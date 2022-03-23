/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/mount.h>
#include <linux/magic.h>

#include "alloc-util.h"
#include "chase-symlinks.h"
#include "escape.h"
#include "fd-util.h"
#include "format-util.h"
#include "fs-util.h"
#include "label.h"
#include "mkdir-label.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "nspawn-mount.h"
#include "parse-util.h"
#include "path-util.h"
#include "rm-rf.h"
#include "set.h"
#include "sort-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "tmpfile-util.h"
#include "user-util.h"

CustomMount* custom_mount_add(CustomMount **l, size_t *n, CustomMountType t) {
        CustomMount *c, *ret;

        assert(l);
        assert(n);
        assert(t >= 0);
        assert(t < _CUSTOM_MOUNT_TYPE_MAX);

        c = reallocarray(*l, *n + 1, sizeof(CustomMount));
        if (!c)
                return NULL;

        *l = c;
        ret = *l + *n;
        (*n)++;

        *ret = (CustomMount) {
                .type = t
        };

        return ret;
}

void custom_mount_free_all(CustomMount *l, size_t n) {
        for (size_t i = 0; i < n; i++) {
                CustomMount *m = l + i;

                free(m->source);
                free(m->destination);
                free(m->options);

                if (m->work_dir) {
                        (void) rm_rf(m->work_dir, REMOVE_ROOT|REMOVE_PHYSICAL);
                        free(m->work_dir);
                }

                if (m->rm_rf_tmpdir) {
                        (void) rm_rf(m->rm_rf_tmpdir, REMOVE_ROOT|REMOVE_PHYSICAL);
                        free(m->rm_rf_tmpdir);
                }

                strv_free(m->lower);
                free(m->type_argument);
        }

        free(l);
}

static int custom_mount_compare(const CustomMount *a, const CustomMount *b) {
        int r;

        r = path_compare(a->destination, b->destination);
        if (r != 0)
                return r;

        return CMP(a->type, b->type);
}

static bool source_path_is_valid(const char *p) {
        assert(p);

        if (*p == '+')
                p++;

        return path_is_absolute(p);
}

static char *resolve_source_path(const char *dest, const char *source) {
        if (!source)
                return NULL;

        if (source[0] == '+')
                return path_join(dest, source + 1);

        return strdup(source);
}

static int allocate_temporary_source(CustomMount *m) {
        assert(m);
        assert(!m->source);
        assert(!m->rm_rf_tmpdir);

        m->rm_rf_tmpdir = strdup("/var/tmp/nspawn-temp-XXXXXX");
        if (!m->rm_rf_tmpdir)
                return log_oom();

        if (!mkdtemp(m->rm_rf_tmpdir)) {
                m->rm_rf_tmpdir = mfree(m->rm_rf_tmpdir);
                return log_error_errno(errno, "Failed to acquire temporary directory: %m");
        }

        m->source = path_join(m->rm_rf_tmpdir, "src");
        if (!m->source)
                return log_oom();

        if (mkdir(m->source, 0755) < 0)
                return log_error_errno(errno, "Failed to create %s: %m", m->source);

        return 0;
}

int custom_mount_prepare_all(const char *dest, CustomMount *l, size_t n) {
        int r;

        /* Prepare all custom mounts. This will make source we know all temporary directories. This is called in the
         * parent process, so that we know the temporary directories to remove on exit before we fork off the
         * children. */

        assert(l || n == 0);

        /* Order the custom mounts, and make sure we have a working directory */
        typesafe_qsort(l, n, custom_mount_compare);

        for (size_t i = 0; i < n; i++) {
                CustomMount *m = l + i;

                /* /proc we mount in the inner child, i.e. when we acquired CLONE_NEWPID. All other mounts we mount
                 * already in the outer child, so that the mounts are already established before CLONE_NEWPID and in
                 * particular CLONE_NEWUSER. This also means any custom mounts below /proc also need to be mounted in
                 * the inner child, not the outer one. Determine this here. */
                m->in_userns = path_startswith(m->destination, "/proc");

                if (m->type == CUSTOM_MOUNT_BIND) {
                        if (m->source) {
                                char *s;

                                s = resolve_source_path(dest, m->source);
                                if (!s)
                                        return log_oom();

                                free_and_replace(m->source, s);
                        } else {
                                /* No source specified? In that case, use a throw-away temporary directory in /var/tmp */

                                r = allocate_temporary_source(m);
                                if (r < 0)
                                        return r;
                        }
                }

                if (m->type == CUSTOM_MOUNT_OVERLAY) {
                        char **j;

                        STRV_FOREACH(j, m->lower) {
                                char *s;

                                s = resolve_source_path(dest, *j);
                                if (!s)
                                        return log_oom();

                                free_and_replace(*j, s);
                        }

                        if (m->source) {
                                char *s;

                                s = resolve_source_path(dest, m->source);
                                if (!s)
                                        return log_oom();

                                free_and_replace(m->source, s);
                        } else {
                                r = allocate_temporary_source(m);
                                if (r < 0)
                                        return r;
                        }

                        if (m->work_dir) {
                                char *s;

                                s = resolve_source_path(dest, m->work_dir);
                                if (!s)
                                        return log_oom();

                                free_and_replace(m->work_dir, s);
                        } else {
                                r = tempfn_random(m->source, NULL, &m->work_dir);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to acquire working directory: %m");
                        }

                        (void) mkdir_label(m->work_dir, 0700);
                }
        }

        return 0;
}

int bind_mount_parse(CustomMount **l, size_t *n, const char *s, bool read_only) {
        _cleanup_free_ char *source = NULL, *destination = NULL, *opts = NULL;
        const char *p = s;
        CustomMount *m;
        int r;

        assert(l);
        assert(n);

        r = extract_many_words(&p, ":", EXTRACT_DONT_COALESCE_SEPARATORS, &source, &destination, NULL);
        if (r < 0)
                return r;
        if (r == 0)
                return -EINVAL;
        if (r == 1) {
                destination = strdup(source[0] == '+' ? source+1 : source);
                if (!destination)
                        return -ENOMEM;
        }
        if (r == 2 && !isempty(p)) {
                opts = strdup(p);
                if (!opts)
                        return -ENOMEM;
        }

        if (isempty(source))
                source = mfree(source);
        else if (!source_path_is_valid(source))
                return -EINVAL;

        if (!path_is_absolute(destination))
                return -EINVAL;

        m = custom_mount_add(l, n, CUSTOM_MOUNT_BIND);
        if (!m)
                return -ENOMEM;

        m->source = TAKE_PTR(source);
        m->destination = TAKE_PTR(destination);
        m->read_only = read_only;
        m->options = TAKE_PTR(opts);

        return 0;
}

int tmpfs_mount_parse(CustomMount **l, size_t *n, const char *s) {
        _cleanup_free_ char *path = NULL, *opts = NULL;
        const char *p = s;
        CustomMount *m;
        int r;

        assert(l);
        assert(n);
        assert(s);

        r = extract_first_word(&p, &path, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (r < 0)
                return r;
        if (r == 0)
                return -EINVAL;

        if (isempty(p))
                opts = strdup("mode=0755");
        else
                opts = strdup(p);
        if (!opts)
                return -ENOMEM;

        if (!path_is_absolute(path))
                return -EINVAL;

        m = custom_mount_add(l, n, CUSTOM_MOUNT_TMPFS);
        if (!m)
                return -ENOMEM;

        m->destination = TAKE_PTR(path);
        m->options = TAKE_PTR(opts);

        return 0;
}

int overlay_mount_parse(CustomMount **l, size_t *n, const char *s, bool read_only) {
        _cleanup_free_ char *upper = NULL, *destination = NULL;
        _cleanup_strv_free_ char **lower = NULL;
        CustomMount *m;
        int k;

        k = strv_split_full(&lower, s, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (k < 0)
                return k;
        if (k < 2)
                return -EADDRNOTAVAIL;
        if (k == 2) {
                /* If two parameters are specified, the first one is the lower, the second one the upper directory. And
                 * we'll also define the destination mount point the same as the upper. */

                if (!source_path_is_valid(lower[0]) ||
                    !source_path_is_valid(lower[1]))
                        return -EINVAL;

                upper = TAKE_PTR(lower[1]);

                destination = strdup(upper[0] == '+' ? upper+1 : upper); /* take the destination without "+" prefix */
                if (!destination)
                        return -ENOMEM;
        } else {
                char **i;

                /* If more than two parameters are specified, the last one is the destination, the second to last one
                 * the "upper", and all before that the "lower" directories. */

                destination = lower[k - 1];
                upper = TAKE_PTR(lower[k - 2]);

                STRV_FOREACH(i, lower)
                        if (!source_path_is_valid(*i))
                                return -EINVAL;

                /* If the upper directory is unspecified, then let's create it automatically as a throw-away directory
                 * in /var/tmp */
                if (isempty(upper))
                        upper = mfree(upper);
                else if (!source_path_is_valid(upper))
                        return -EINVAL;

                if (!path_is_absolute(destination))
                        return -EINVAL;
        }

        m = custom_mount_add(l, n, CUSTOM_MOUNT_OVERLAY);
        if (!m)
                return -ENOMEM;

        m->destination = TAKE_PTR(destination);
        m->source = TAKE_PTR(upper);
        m->lower = TAKE_PTR(lower);
        m->read_only = read_only;

        return 0;
}

int inaccessible_mount_parse(CustomMount **l, size_t *n, const char *s) {
        _cleanup_free_ char *path = NULL;
        CustomMount *m;

        assert(l);
        assert(n);
        assert(s);

        if (!path_is_absolute(s))
                return -EINVAL;

        path = strdup(s);
        if (!path)
                return -ENOMEM;

        m = custom_mount_add(l, n, CUSTOM_MOUNT_INACCESSIBLE);
        if (!m)
                return -ENOMEM;

        m->destination = TAKE_PTR(path);
        return 0;
}

int tmpfs_patch_options(
                const char *options,
                uid_t uid_shift,
                const char *selinux_apifs_context,
                char **ret) {

        _cleanup_free_ char *buf = NULL;

        assert(ret);

        if (options) {
                buf = strdup(options);
                if (!buf)
                        return -ENOMEM;
        }

        if (uid_shift != UID_INVALID)
                if (strextendf_with_separator(&buf, ",", "uid=" UID_FMT ",gid=" UID_FMT, uid_shift, uid_shift) < 0)
                        return -ENOMEM;

#if HAVE_SELINUX
        if (selinux_apifs_context)
                if (strextendf_with_separator(&buf, ",", "context=\"%s\"", selinux_apifs_context) < 0)
                        return -ENOMEM;
#endif

        *ret = TAKE_PTR(buf);
        return !!*ret;
}

int mount_sysfs(const char *dest, MountSettingsMask mount_settings) {
        const char *full, *top;
        int r;
        unsigned long extra_flags = 0;

        top = prefix_roota(dest, "/sys");
        r = path_is_fs_type(top, SYSFS_MAGIC);
        if (r < 0)
                return log_error_errno(r, "Failed to determine filesystem type of %s: %m", top);
        /* /sys might already be mounted as sysfs by the outer child in the
         * !netns case. In this case, it's all good. Don't touch it because we
         * don't have the right to do so, see https://github.com/systemd/systemd/issues/1555.
         */
        if (r > 0)
                return 0;

        full = prefix_roota(top, "/full");

        (void) mkdir(full, 0755);

        if (FLAGS_SET(mount_settings, MOUNT_APPLY_APIVFS_RO))
                extra_flags |= MS_RDONLY;

        r = mount_nofollow_verbose(LOG_ERR, "sysfs", full, "sysfs",
                                   MS_NOSUID|MS_NOEXEC|MS_NODEV|extra_flags, NULL);
        if (r < 0)
                return r;

        FOREACH_STRING(x, "block", "bus", "class", "dev", "devices", "kernel") {
                _cleanup_free_ char *from = NULL, *to = NULL;

                from = path_join(full, x);
                if (!from)
                        return log_oom();

                to = path_join(top, x);
                if (!to)
                        return log_oom();

                (void) mkdir(to, 0755);

                r = mount_nofollow_verbose(LOG_ERR, from, to, NULL, MS_BIND, NULL);
                if (r < 0)
                        return r;

                r = mount_nofollow_verbose(LOG_ERR, NULL, to, NULL,
                                           MS_BIND|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT|extra_flags, NULL);
                if (r < 0)
                        return r;
        }

        r = umount_verbose(LOG_ERR, full, UMOUNT_NOFOLLOW);
        if (r < 0)
                return r;

        if (rmdir(full) < 0)
                return log_error_errno(errno, "Failed to remove %s: %m", full);

        /* Create mountpoint for cgroups. Otherwise we are not allowed since we
         * remount /sys read-only.
         */
        const char *x = prefix_roota(top, "/fs/cgroup");
        (void) mkdir_p(x, 0755);

        return mount_nofollow_verbose(LOG_ERR, NULL, top, NULL,
                                      MS_BIND|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT|extra_flags, NULL);
}

int mount_all(const char *dest,
              MountSettingsMask mount_settings,
              uid_t uid_shift,
              const char *selinux_apifs_context) {

#define PROC_INACCESSIBLE_REG(path)                                     \
        { "/run/systemd/inaccessible/reg", (path), NULL, NULL, MS_BIND, \
          MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO }, /* Bind mount first ... */ \
        { NULL, (path), NULL, NULL, MS_BIND|MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT, \
          MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO } /* Then, make it r/o */

#define PROC_READ_ONLY(path)                                            \
        { (path), (path), NULL, NULL, MS_BIND,                          \
          MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO }, /* Bind mount first ... */ \
        { NULL,   (path), NULL, NULL, MS_BIND|MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT, \
          MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO } /* Then, make it r/o */

        typedef struct MountPoint {
                const char *what;
                const char *where;
                const char *type;
                const char *options;
                unsigned long flags;
                MountSettingsMask mount_settings;
        } MountPoint;

        static const MountPoint mount_table[] = {
                /* First we list inner child mounts (i.e. mounts applied *after* entering user namespacing) */
                { "proc",            "/proc",           "proc",  NULL,        MS_NOSUID|MS_NOEXEC|MS_NODEV,
                  MOUNT_FATAL|MOUNT_IN_USERNS|MOUNT_MKDIR|MOUNT_FOLLOW_SYMLINKS }, /* we follow symlinks here since not following them requires /proc/ already being mounted, which we don't have here. */

                { "/proc/sys",       "/proc/sys",       NULL,    NULL,        MS_BIND,
                  MOUNT_FATAL|MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO },                          /* Bind mount first ... */

                { "/proc/sys/net",   "/proc/sys/net",   NULL,    NULL,        MS_BIND,
                  MOUNT_FATAL|MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO|MOUNT_APPLY_APIVFS_NETNS }, /* (except for this) */

                { NULL,              "/proc/sys",       NULL,    NULL,        MS_BIND|MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT,
                  MOUNT_FATAL|MOUNT_IN_USERNS|MOUNT_APPLY_APIVFS_RO },                          /* ... then, make it r/o */

                /* Make these files inaccessible to container payloads: they potentially leak information about kernel
                 * internals or the host's execution environment to the container */
                PROC_INACCESSIBLE_REG("/proc/kallsyms"),
                PROC_INACCESSIBLE_REG("/proc/kcore"),
                PROC_INACCESSIBLE_REG("/proc/keys"),
                PROC_INACCESSIBLE_REG("/proc/sysrq-trigger"),
                PROC_INACCESSIBLE_REG("/proc/timer_list"),

                /* Make these directories read-only to container payloads: they show hardware information, and in some
                 * cases contain tunables the container really shouldn't have access to. */
                PROC_READ_ONLY("/proc/acpi"),
                PROC_READ_ONLY("/proc/apm"),
                PROC_READ_ONLY("/proc/asound"),
                PROC_READ_ONLY("/proc/bus"),
                PROC_READ_ONLY("/proc/fs"),
                PROC_READ_ONLY("/proc/irq"),
                PROC_READ_ONLY("/proc/scsi"),

                { "mqueue",                 "/dev/mqueue",                  "mqueue", NULL,                            MS_NOSUID|MS_NOEXEC|MS_NODEV,
                  MOUNT_IN_USERNS|MOUNT_MKDIR },

                /* Then we list outer child mounts (i.e. mounts applied *before* entering user namespacing) */
                { "tmpfs",                  "/tmp",                         "tmpfs", "mode=1777" NESTED_TMPFS_LIMITS,  MS_NOSUID|MS_NODEV|MS_STRICTATIME,
                  MOUNT_FATAL|MOUNT_APPLY_TMPFS_TMP|MOUNT_MKDIR },
                { "tmpfs",                  "/sys",                         "tmpfs", "mode=555" TMPFS_LIMITS_SYS,      MS_NOSUID|MS_NOEXEC|MS_NODEV,
                  MOUNT_FATAL|MOUNT_APPLY_APIVFS_NETNS|MOUNT_MKDIR },
                { "sysfs",                  "/sys",                         "sysfs", NULL,                             MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV,
                  MOUNT_FATAL|MOUNT_APPLY_APIVFS_RO|MOUNT_MKDIR },    /* skipped if above was mounted */
                { "sysfs",                  "/sys",                         "sysfs", NULL,                             MS_NOSUID|MS_NOEXEC|MS_NODEV,
                  MOUNT_FATAL|MOUNT_MKDIR },                          /* skipped if above was mounted */
                { "tmpfs",                  "/dev",                         "tmpfs", "mode=755" TMPFS_LIMITS_DEV,      MS_NOSUID|MS_STRICTATIME,
                  MOUNT_FATAL|MOUNT_MKDIR },
                { "tmpfs",                  "/dev/shm",                     "tmpfs", "mode=1777" NESTED_TMPFS_LIMITS,  MS_NOSUID|MS_NODEV|MS_STRICTATIME,
                  MOUNT_FATAL|MOUNT_MKDIR },
                { "tmpfs",                  "/run",                         "tmpfs", "mode=755" TMPFS_LIMITS_RUN,      MS_NOSUID|MS_NODEV|MS_STRICTATIME,
                  MOUNT_FATAL|MOUNT_MKDIR },
                { "/run/host",              "/run/host",                    NULL,    NULL,                             MS_BIND,
                  MOUNT_FATAL|MOUNT_MKDIR|MOUNT_PREFIX_ROOT }, /* Prepare this so that we can make it read-only when we are done */
                { "/etc/os-release",        "/run/host/os-release",         NULL,    NULL,                             MS_BIND,
                  MOUNT_TOUCH }, /* As per kernel interface requirements, bind mount first (creating mount points) and make read-only later */
                { "/usr/lib/os-release",    "/run/host/os-release",         NULL,    NULL,                             MS_BIND,
                  MOUNT_FATAL }, /* If /etc/os-release doesn't exist use the version in /usr/lib as fallback */
                { NULL,                     "/run/host/os-release",         NULL,    NULL,                             MS_BIND|MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT,
                  MOUNT_FATAL },
                { NULL,                     "/run/host",                    NULL,    NULL,                             MS_BIND|MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT,
                  MOUNT_FATAL|MOUNT_IN_USERNS },
#if HAVE_SELINUX
                { "/sys/fs/selinux",        "/sys/fs/selinux",              NULL,    NULL,                             MS_BIND,
                  MOUNT_MKDIR },  /* Bind mount first (mkdir/chown the mount point in case /sys/ is mounted as minimal skeleton tmpfs) */
                { NULL,                     "/sys/fs/selinux",              NULL,    NULL,                             MS_BIND|MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_REMOUNT,
                  0 },            /* Then, make it r/o (don't mkdir/chown the mount point here, the previous entry already did that) */
#endif
        };

        bool use_userns = FLAGS_SET(mount_settings, MOUNT_USE_USERNS);
        bool netns = FLAGS_SET(mount_settings, MOUNT_APPLY_APIVFS_NETNS);
        bool ro = FLAGS_SET(mount_settings, MOUNT_APPLY_APIVFS_RO);
        bool in_userns = FLAGS_SET(mount_settings, MOUNT_IN_USERNS);
        bool tmpfs_tmp = FLAGS_SET(mount_settings, MOUNT_APPLY_TMPFS_TMP);
        int r;

        for (size_t k = 0; k < ELEMENTSOF(mount_table); k++) {
                _cleanup_free_ char *where = NULL, *options = NULL, *prefixed = NULL;
                bool fatal = FLAGS_SET(mount_table[k].mount_settings, MOUNT_FATAL);
                const char *o;

                if (in_userns != FLAGS_SET(mount_table[k].mount_settings, MOUNT_IN_USERNS))
                        continue;

                if (!netns && FLAGS_SET(mount_table[k].mount_settings, MOUNT_APPLY_APIVFS_NETNS))
                        continue;

                if (!ro && FLAGS_SET(mount_table[k].mount_settings, MOUNT_APPLY_APIVFS_RO))
                        continue;

                if (!tmpfs_tmp && FLAGS_SET(mount_table[k].mount_settings, MOUNT_APPLY_TMPFS_TMP))
                        continue;

                r = chase_symlinks(mount_table[k].where, dest, CHASE_NONEXISTENT|CHASE_PREFIX_ROOT, &where, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve %s/%s: %m", dest, mount_table[k].where);

                /* Skip this entry if it is not a remount. */
                if (mount_table[k].what) {
                        r = path_is_mount_point(where, NULL, 0);
                        if (r < 0 && r != -ENOENT)
                                return log_error_errno(r, "Failed to detect whether %s is a mount point: %m", where);
                        if (r > 0)
                                continue;
                }

                if ((mount_table[k].mount_settings & (MOUNT_MKDIR|MOUNT_TOUCH)) != 0) {
                        uid_t u = (use_userns && !in_userns) ? uid_shift : UID_INVALID;

                        if (FLAGS_SET(mount_table[k].mount_settings, MOUNT_TOUCH))
                                r = mkdir_parents_safe(dest, where, 0755, u, u, 0);
                        else
                                r = mkdir_p_safe(dest, where, 0755, u, u, 0);
                        if (r < 0 && r != -EEXIST) {
                                if (fatal && r != -EROFS)
                                        return log_error_errno(r, "Failed to create directory %s: %m", where);

                                log_debug_errno(r, "Failed to create directory %s: %m", where);

                                /* If we failed mkdir() or chown() due to the root directory being read only,
                                 * attempt to mount this fs anyway and let mount_verbose log any errors */
                                if (r != -EROFS)
                                        continue;
                        }
                }

                if (FLAGS_SET(mount_table[k].mount_settings, MOUNT_TOUCH)) {
                        r = touch(where);
                        if (r < 0 && r != -EEXIST) {
                                if (fatal && r != -EROFS)
                                        return log_error_errno(r, "Failed to create file %s: %m", where);

                                log_debug_errno(r, "Failed to create file %s: %m", where);
                                if (r != -EROFS)
                                        continue;
                        }
                }

                o = mount_table[k].options;
                if (streq_ptr(mount_table[k].type, "tmpfs")) {
                        r = tmpfs_patch_options(o, in_userns ? 0 : uid_shift, selinux_apifs_context, &options);
                        if (r < 0)
                                return log_oom();
                        if (r > 0)
                                o = options;
                }

                if (FLAGS_SET(mount_table[k].mount_settings, MOUNT_PREFIX_ROOT)) {
                        /* Optionally prefix the mount source with the root dir. This is useful in bind
                         * mounts to be created within the container image before we transition into it. Note
                         * that MOUNT_IN_USERNS is run after we transitioned hence prefixing is not ncessary
                         * for those. */
                        r = chase_symlinks(mount_table[k].what, dest, CHASE_PREFIX_ROOT, &prefixed, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to resolve %s/%s: %m", dest, mount_table[k].what);
                }

                r = mount_verbose_full(
                                fatal ? LOG_ERR : LOG_DEBUG,
                                prefixed ?: mount_table[k].what,
                                where,
                                mount_table[k].type,
                                mount_table[k].flags,
                                o,
                                FLAGS_SET(mount_table[k].mount_settings, MOUNT_FOLLOW_SYMLINKS));
                if (r < 0 && fatal)
                        return r;
        }

        return 0;
}

static int parse_mount_bind_options(const char *options, unsigned long *mount_flags, char **mount_opts, bool *idmapped) {
        unsigned long flags = *mount_flags;
        char *opts = NULL;
        bool flag_idmapped = *idmapped;
        int r;

        assert(options);

        for (;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&options, &word, ",", 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to extract mount option: %m");
                if (r == 0)
                        break;

                if (streq(word, "rbind"))
                        flags |= MS_REC;
                else if (streq(word, "norbind"))
                        flags &= ~MS_REC;
                else if (streq(word, "idmap"))
                        flag_idmapped = true;
                else if (streq(word, "noidmap"))
                        flag_idmapped = false;
                else
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Invalid bind mount option: %s", word);
        }

        *mount_flags = flags;
        *idmapped = flag_idmapped;
        /* in the future mount_opts will hold string options for mount(2) */
        *mount_opts = opts;

        return 0;
}

static int mount_bind(const char *dest, CustomMount *m, uid_t uid_shift, uid_t uid_range) {
        _cleanup_free_ char *mount_opts = NULL, *where = NULL;
        unsigned long mount_flags = MS_BIND | MS_REC;
        struct stat source_st, dest_st;
        int r;
        bool idmapped = false;

        assert(dest);
        assert(m);

        if (m->options) {
                r = parse_mount_bind_options(m->options, &mount_flags, &mount_opts, &idmapped);
                if (r < 0)
                        return r;
        }

        /* If this is a bind mount from a temporary sources change ownership of the source to the container's
         * root UID. Otherwise it would always show up as "nobody" if user namespacing is used. */
        if (m->rm_rf_tmpdir && chown(m->source, uid_shift, uid_shift) < 0)
                return log_error_errno(errno, "Failed to chown %s: %m", m->source);

        if (stat(m->source, &source_st) < 0)
                return log_error_errno(errno, "Failed to stat %s: %m", m->source);

        r = chase_symlinks(m->destination, dest, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &where, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to resolve %s/%s: %m", dest, m->destination);
        if (r > 0) { /* Path exists already? */

                if (stat(where, &dest_st) < 0)
                        return log_error_errno(errno, "Failed to stat %s: %m", where);

                if (S_ISDIR(source_st.st_mode) && !S_ISDIR(dest_st.st_mode))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Cannot bind mount directory %s on file %s.",
                                               m->source, where);

                if (!S_ISDIR(source_st.st_mode) && S_ISDIR(dest_st.st_mode))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Cannot bind mount file %s on directory %s.",
                                               m->source, where);

        } else { /* Path doesn't exist yet? */
                r = mkdir_parents_label(where, 0755);
                if (r < 0)
                        return log_error_errno(r, "Failed to make parents of %s: %m", where);

                /* Create the mount point. Any non-directory file can be
                * mounted on any non-directory file (regular, fifo, socket,
                * char, block).
                */
                if (S_ISDIR(source_st.st_mode))
                        r = mkdir_label(where, 0755);
                else
                        r = touch(where);
                if (r < 0)
                        return log_error_errno(r, "Failed to create mount point %s: %m", where);
        }

        r = mount_nofollow_verbose(LOG_ERR, m->source, where, NULL, mount_flags, mount_opts);
        if (r < 0)
                return r;

        if (m->read_only) {
                r = bind_remount_recursive(where, MS_RDONLY, MS_RDONLY, NULL);
                if (r < 0)
                        return log_error_errno(r, "Read-only bind mount failed: %m");
        }

        if (idmapped) {
                r = remount_idmap(where, uid_shift, uid_range);
                if (r < 0)
                        return log_error_errno(r, "Failed to map ids for bind mount %s: %m", where);
        }

        return 0;
}

static int mount_tmpfs(const char *dest, CustomMount *m, uid_t uid_shift, const char *selinux_apifs_context) {
        const char *options;
        _cleanup_free_ char *buf = NULL, *where = NULL;
        int r;

        assert(dest);
        assert(m);

        r = chase_symlinks(m->destination, dest, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &where, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to resolve %s/%s: %m", dest, m->destination);
        if (r == 0) { /* Doesn't exist yet? */
                r = mkdir_p_label(where, 0755);
                if (r < 0)
                        return log_error_errno(r, "Creating mount point for tmpfs %s failed: %m", where);
        }

        r = tmpfs_patch_options(m->options, uid_shift == 0 ? UID_INVALID : uid_shift, selinux_apifs_context, &buf);
        if (r < 0)
                return log_oom();
        options = r > 0 ? buf : m->options;

        return mount_nofollow_verbose(LOG_ERR, "tmpfs", where, "tmpfs", MS_NODEV|MS_STRICTATIME, options);
}

static char *joined_and_escaped_lower_dirs(char **lower) {
        _cleanup_strv_free_ char **sv = NULL;

        sv = strv_copy(lower);
        if (!sv)
                return NULL;

        strv_reverse(sv);

        if (!strv_shell_escape(sv, ",:"))
                return NULL;

        return strv_join(sv, ":");
}

static int mount_overlay(const char *dest, CustomMount *m) {
        _cleanup_free_ char *lower = NULL, *where = NULL, *escaped_source = NULL;
        const char *options;
        int r;

        assert(dest);
        assert(m);

        r = chase_symlinks(m->destination, dest, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &where, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to resolve %s/%s: %m", dest, m->destination);
        if (r == 0) { /* Doesn't exist yet? */
                r = mkdir_label(where, 0755);
                if (r < 0)
                        return log_error_errno(r, "Creating mount point for overlay %s failed: %m", where);
        }

        (void) mkdir_p_label(m->source, 0755);

        lower = joined_and_escaped_lower_dirs(m->lower);
        if (!lower)
                return log_oom();

        escaped_source = shell_escape(m->source, ",:");
        if (!escaped_source)
                return log_oom();

        if (m->read_only)
                options = strjoina("lowerdir=", escaped_source, ":", lower);
        else {
                _cleanup_free_ char *escaped_work_dir = NULL;

                escaped_work_dir = shell_escape(m->work_dir, ",:");
                if (!escaped_work_dir)
                        return log_oom();

                options = strjoina("lowerdir=", lower, ",upperdir=", escaped_source, ",workdir=", escaped_work_dir);
        }

        return mount_nofollow_verbose(LOG_ERR, "overlay", where, "overlay", m->read_only ? MS_RDONLY : 0, options);
}

static int mount_inaccessible(const char *dest, CustomMount *m) {
        _cleanup_free_ char *where = NULL, *source = NULL;
        struct stat st;
        int r;

        assert(dest);
        assert(m);

        r = chase_symlinks_and_stat(m->destination, dest, CHASE_PREFIX_ROOT, &where, &st, NULL);
        if (r < 0) {
                log_full_errno(m->graceful ? LOG_DEBUG : LOG_ERR, r, "Failed to resolve %s/%s: %m", dest, m->destination);
                return m->graceful ? 0 : r;
        }

        r = mode_to_inaccessible_node(NULL, st.st_mode, &source);
        if (r < 0)
                return m->graceful ? 0 : r;

        r = mount_nofollow_verbose(m->graceful ? LOG_DEBUG : LOG_ERR, source, where, NULL, MS_BIND, NULL);
        if (r < 0)
                return m->graceful ? 0 : r;

        r = mount_nofollow_verbose(m->graceful ? LOG_DEBUG : LOG_ERR, NULL, where, NULL, MS_BIND|MS_RDONLY|MS_REMOUNT, NULL);
        if (r < 0) {
                (void) umount_verbose(m->graceful ? LOG_DEBUG : LOG_ERR, where, UMOUNT_NOFOLLOW);
                return m->graceful ? 0 : r;
        }

        return 0;
}

static int mount_arbitrary(const char *dest, CustomMount *m) {
        _cleanup_free_ char *where = NULL;
        int r;

        assert(dest);
        assert(m);

        r = chase_symlinks(m->destination, dest, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &where, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to resolve %s/%s: %m", dest, m->destination);
        if (r == 0) { /* Doesn't exist yet? */
                r = mkdir_p_label(where, 0755);
                if (r < 0)
                        return log_error_errno(r, "Creating mount point for mount %s failed: %m", where);
        }

        return mount_nofollow_verbose(LOG_ERR, m->source, where, m->type_argument, 0, m->options);
}

int mount_custom(
                const char *dest,
                CustomMount *mounts, size_t n,
                uid_t uid_shift,
                uid_t uid_range,
                const char *selinux_apifs_context,
                MountSettingsMask mount_settings) {
        int r;

        assert(dest);

        for (size_t i = 0; i < n; i++) {
                CustomMount *m = mounts + i;

                if (FLAGS_SET(mount_settings, MOUNT_IN_USERNS) != m->in_userns)
                        continue;

                if (FLAGS_SET(mount_settings, MOUNT_ROOT_ONLY) && !path_equal(m->destination, "/"))
                        continue;

                if (FLAGS_SET(mount_settings, MOUNT_NON_ROOT_ONLY) && path_equal(m->destination, "/"))
                        continue;

                switch (m->type) {

                case CUSTOM_MOUNT_BIND:
                        r = mount_bind(dest, m, uid_shift, uid_range);
                        break;

                case CUSTOM_MOUNT_TMPFS:
                        r = mount_tmpfs(dest, m, uid_shift, selinux_apifs_context);
                        break;

                case CUSTOM_MOUNT_OVERLAY:
                        r = mount_overlay(dest, m);
                        break;

                case CUSTOM_MOUNT_INACCESSIBLE:
                        r = mount_inaccessible(dest, m);
                        break;

                case CUSTOM_MOUNT_ARBITRARY:
                        r = mount_arbitrary(dest, m);
                        break;

                default:
                        assert_not_reached();
                }

                if (r < 0)
                        return r;
        }

        return 0;
}

bool has_custom_root_mount(const CustomMount *mounts, size_t n) {
        for (size_t i = 0; i < n; i++)
                if (path_equal(mounts[i].destination, "/"))
                        return true;

        return false;
}

static int setup_volatile_state(const char *directory, uid_t uid_shift, const char *selinux_apifs_context) {
        _cleanup_free_ char *buf = NULL;
        const char *p, *options;
        int r;

        assert(directory);

        /* --volatile=state means we simply overmount /var with a tmpfs, and the rest read-only. */

        r = bind_remount_recursive(directory, MS_RDONLY, MS_RDONLY, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to remount %s read-only: %m", directory);

        p = prefix_roota(directory, "/var");
        r = mkdir(p, 0755);
        if (r < 0 && errno != EEXIST)
                return log_error_errno(errno, "Failed to create %s: %m", directory);

        options = "mode=755" TMPFS_LIMITS_VOLATILE_STATE;
        r = tmpfs_patch_options(options, uid_shift == 0 ? UID_INVALID : uid_shift, selinux_apifs_context, &buf);
        if (r < 0)
                return log_oom();
        if (r > 0)
                options = buf;

        return mount_nofollow_verbose(LOG_ERR, "tmpfs", p, "tmpfs", MS_STRICTATIME, options);
}

static int setup_volatile_yes(const char *directory, uid_t uid_shift, const char *selinux_apifs_context) {
        bool tmpfs_mounted = false, bind_mounted = false;
        char template[] = "/tmp/nspawn-volatile-XXXXXX";
        _cleanup_free_ char *buf = NULL, *bindir = NULL;
        const char *f, *t, *options;
        struct stat st;
        int r;

        assert(directory);

        /* --volatile=yes means we mount a tmpfs to the root dir, and the original /usr to use inside it, and
         * that read-only. Before we start setting this up let's validate if the image has the /usr merge
         * implemented, and let's output a friendly log message if it hasn't. */

        bindir = path_join(directory, "/bin");
        if (!bindir)
                return log_oom();
        if (lstat(bindir, &st) < 0) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to stat /bin directory below image: %m");

                /* ENOENT is fine, just means the image is probably just a naked /usr and we can create the
                 * rest. */
        } else if (S_ISDIR(st.st_mode))
                return log_error_errno(SYNTHETIC_ERRNO(EISDIR),
                                       "Sorry, --volatile=yes mode is not supported with OS images that have not merged /bin/, /sbin/, /lib/, /lib64/ into /usr/. "
                                       "Please work with your distribution and help them adopt the merged /usr scheme.");
        else if (!S_ISLNK(st.st_mode))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Error starting image: if --volatile=yes is used /bin must be a symlink (for merged /usr support) or non-existent (in which case a symlink is created automatically).");

        if (!mkdtemp(template))
                return log_error_errno(errno, "Failed to create temporary directory: %m");

        options = "mode=755" TMPFS_LIMITS_ROOTFS;
        r = tmpfs_patch_options(options, uid_shift == 0 ? UID_INVALID : uid_shift, selinux_apifs_context, &buf);
        if (r < 0)
                goto fail;
        if (r > 0)
                options = buf;

        r = mount_nofollow_verbose(LOG_ERR, "tmpfs", template, "tmpfs", MS_STRICTATIME, options);
        if (r < 0)
                goto fail;

        tmpfs_mounted = true;

        f = prefix_roota(directory, "/usr");
        t = prefix_roota(template, "/usr");

        r = mkdir(t, 0755);
        if (r < 0 && errno != EEXIST) {
                r = log_error_errno(errno, "Failed to create %s: %m", t);
                goto fail;
        }

        r = mount_nofollow_verbose(LOG_ERR, f, t, NULL, MS_BIND|MS_REC, NULL);
        if (r < 0)
                goto fail;

        bind_mounted = true;

        r = bind_remount_recursive(t, MS_RDONLY, MS_RDONLY, NULL);
        if (r < 0) {
                log_error_errno(r, "Failed to remount %s read-only: %m", t);
                goto fail;
        }

        r = mount_nofollow_verbose(LOG_ERR, template, directory, NULL, MS_MOVE, NULL);
        if (r < 0)
                goto fail;

        (void) rmdir(template);

        return 0;

fail:
        if (bind_mounted)
                (void) umount_verbose(LOG_ERR, t, UMOUNT_NOFOLLOW);

        if (tmpfs_mounted)
                (void) umount_verbose(LOG_ERR, template, UMOUNT_NOFOLLOW);

        (void) rmdir(template);
        return r;
}

static int setup_volatile_overlay(const char *directory, uid_t uid_shift, const char *selinux_apifs_context) {
        _cleanup_free_ char *buf = NULL, *escaped_directory = NULL, *escaped_upper = NULL, *escaped_work = NULL;
        char template[] = "/tmp/nspawn-volatile-XXXXXX";
        const char *upper, *work, *options;
        bool tmpfs_mounted = false;
        int r;

        assert(directory);

        /* --volatile=overlay means we mount an overlayfs to the root dir. */

        if (!mkdtemp(template))
                return log_error_errno(errno, "Failed to create temporary directory: %m");

        options = "mode=755" TMPFS_LIMITS_ROOTFS;
        r = tmpfs_patch_options(options, uid_shift == 0 ? UID_INVALID : uid_shift, selinux_apifs_context, &buf);
        if (r < 0)
                goto finish;
        if (r > 0)
                options = buf;

        r = mount_nofollow_verbose(LOG_ERR, "tmpfs", template, "tmpfs", MS_STRICTATIME, options);
        if (r < 0)
                goto finish;

        tmpfs_mounted = true;

        upper = strjoina(template, "/upper");
        work = strjoina(template, "/work");

        if (mkdir(upper, 0755) < 0) {
                r = log_error_errno(errno, "Failed to create %s: %m", upper);
                goto finish;
        }
        if (mkdir(work, 0755) < 0) {
                r = log_error_errno(errno, "Failed to create %s: %m", work);
                goto finish;
        }

        /* And now, let's overmount the root dir with an overlayfs that uses the root dir as lower dir. It's kinda nice
         * that the kernel allows us to do that without going through some mount point rearrangements. */

        escaped_directory = shell_escape(directory, ",:");
        escaped_upper = shell_escape(upper, ",:");
        escaped_work = shell_escape(work, ",:");
        if (!escaped_directory || !escaped_upper || !escaped_work) {
                r = -ENOMEM;
                goto finish;
        }

        options = strjoina("lowerdir=", escaped_directory, ",upperdir=", escaped_upper, ",workdir=", escaped_work);
        r = mount_nofollow_verbose(LOG_ERR, "overlay", directory, "overlay", 0, options);

finish:
        if (tmpfs_mounted)
                (void) umount_verbose(LOG_ERR, template, UMOUNT_NOFOLLOW);

        (void) rmdir(template);
        return r;
}

int setup_volatile_mode(
                const char *directory,
                VolatileMode mode,
                uid_t uid_shift,
                const char *selinux_apifs_context) {

        switch (mode) {

        case VOLATILE_YES:
                return setup_volatile_yes(directory, uid_shift, selinux_apifs_context);

        case VOLATILE_STATE:
                return setup_volatile_state(directory, uid_shift, selinux_apifs_context);

        case VOLATILE_OVERLAY:
                return setup_volatile_overlay(directory, uid_shift, selinux_apifs_context);

        default:
                return 0;
        }
}

/* Expects *pivot_root_new and *pivot_root_old to be initialised to allocated memory or NULL. */
int pivot_root_parse(char **pivot_root_new, char **pivot_root_old, const char *s) {
        _cleanup_free_ char *root_new = NULL, *root_old = NULL;
        const char *p = s;
        int r;

        assert(pivot_root_new);
        assert(pivot_root_old);

        r = extract_first_word(&p, &root_new, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (r < 0)
                return r;
        if (r == 0)
                return -EINVAL;

        if (isempty(p))
                root_old = NULL;
        else {
                root_old = strdup(p);
                if (!root_old)
                        return -ENOMEM;
        }

        if (!path_is_absolute(root_new))
                return -EINVAL;
        if (root_old && !path_is_absolute(root_old))
                return -EINVAL;

        free_and_replace(*pivot_root_new, root_new);
        free_and_replace(*pivot_root_old, root_old);

        return 0;
}

int setup_pivot_root(const char *directory, const char *pivot_root_new, const char *pivot_root_old) {
        _cleanup_free_ char *directory_pivot_root_new = NULL;
        _cleanup_free_ char *pivot_tmp_pivot_root_old = NULL;
        char pivot_tmp[] = "/tmp/nspawn-pivot-XXXXXX";
        bool remove_pivot_tmp = false;
        int r;

        assert(directory);

        if (!pivot_root_new)
                return 0;

        /* Pivot pivot_root_new to / and the existing / to pivot_root_old.
         * If pivot_root_old is NULL, the existing / disappears.
         * This requires a temporary directory, pivot_tmp, which is
         * not a child of either.
         *
         * This is typically used for OSTree-style containers, where
         * the root partition contains several sysroots which could be
         * run. Normally, one would be chosen by the bootloader and
         * pivoted to / by initramfs.
         *
         * For example, for an OSTree deployment, pivot_root_new
         * would be: /ostree/deploy/$os/deploy/$checksum. Note that this
         * code doesn’t do the /var mount which OSTree expects: use
         * --bind +/sysroot/ostree/deploy/$os/var:/var for that.
         *
         * So in the OSTree case, we’ll end up with something like:
         *  - directory = /tmp/nspawn-root-123456
         *  - pivot_root_new = /ostree/deploy/os/deploy/123abc
         *  - pivot_root_old = /sysroot
         *  - directory_pivot_root_new =
         *       /tmp/nspawn-root-123456/ostree/deploy/os/deploy/123abc
         *  - pivot_tmp = /tmp/nspawn-pivot-123456
         *  - pivot_tmp_pivot_root_old = /tmp/nspawn-pivot-123456/sysroot
         *
         * Requires all file systems at directory and below to be mounted
         * MS_PRIVATE or MS_SLAVE so they can be moved.
         */
        directory_pivot_root_new = path_join(directory, pivot_root_new);
        if (!directory_pivot_root_new)
                return log_oom();

        /* Remount directory_pivot_root_new to make it movable. */
        r = mount_nofollow_verbose(LOG_ERR, directory_pivot_root_new, directory_pivot_root_new, NULL, MS_BIND, NULL);
        if (r < 0)
                goto done;

        if (pivot_root_old) {
                if (!mkdtemp(pivot_tmp)) {
                        r = log_error_errno(errno, "Failed to create temporary directory: %m");
                        goto done;
                }

                remove_pivot_tmp = true;
                pivot_tmp_pivot_root_old = path_join(pivot_tmp, pivot_root_old);
                if (!pivot_tmp_pivot_root_old) {
                        r = log_oom();
                        goto done;
                }

                r = mount_nofollow_verbose(LOG_ERR, directory_pivot_root_new, pivot_tmp, NULL, MS_MOVE, NULL);
                if (r < 0)
                        goto done;

                r = mount_nofollow_verbose(LOG_ERR, directory, pivot_tmp_pivot_root_old, NULL, MS_MOVE, NULL);
                if (r < 0)
                        goto done;

                r = mount_nofollow_verbose(LOG_ERR, pivot_tmp, directory, NULL, MS_MOVE, NULL);
                if (r < 0)
                        goto done;
        } else {
                r = mount_nofollow_verbose(LOG_ERR, directory_pivot_root_new, directory, NULL, MS_MOVE, NULL);
                if (r < 0)
                        goto done;
        }

done:
        if (remove_pivot_tmp)
                (void) rmdir(pivot_tmp);

        return r;
}
