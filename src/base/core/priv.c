#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_SYS_IO_H
#include <sys/io.h>
#endif
#include "emu.h"
#include "priv.h"
#include "dosemu_config.h"
#include "utilities.h"
#ifdef X86_EMULATOR
#include "cpu-emu.h"
#endif

/* Some handy information to have around */
static uid_t uid,euid;
static gid_t gid,egid;
static uid_t cur_euid;
static gid_t cur_egid;

static int skip_priv_setting = 0;

int can_do_root_stuff;
int under_root_login;
int using_sudo;
int current_iopl;

#define PRIVS_ARE_ON (euid == cur_euid)
#define PRIVS_ARE_OFF (uid == cur_euid)

static int _priv_on(void)
{
  if (seteuid(euid)) {
    error("Cannot turn privs on!\n");
    return 0;
  }
  cur_euid = euid;
  if (setegid(egid)) {
    error("Cannot turn privs on!\n");
    return 0;
  }
  cur_egid = egid;
  return 1;
}

static int _priv_off(void)
{
  if (seteuid(uid)) {
    error("Cannot turn privs off!\n");
    return 0;
  }
  cur_euid = uid;
  if (setegid(gid)) {
    error("Cannot turn privs off!\n");
    return 0;
  }
  cur_egid = gid;
  return 1;
}

int real_enter_priv_on(void)
{
  if (skip_priv_setting) return 1;
  assert(PRIVS_ARE_OFF);
  return _priv_on();
}

int real_leave_priv_setting(void)
{
  if (skip_priv_setting) return 1;
  assert(PRIVS_ARE_ON);
  return _priv_off();
}

int priv_iopl(int pl)
{
#ifdef HAVE_SYS_IO_H
  int ret;
  assert(PRIVS_ARE_OFF);
  _priv_on();
  ret = iopl(pl);
  _priv_off();
#ifdef X86_EMULATOR
  if (config.cpu_vm == CPUVM_EMU) e_priv_iopl(pl);
#endif
  if (ret == 0)
    current_iopl = pl;
  return ret;
#else
  return -1;
#endif
}

uid_t get_orig_uid(void)
{
  return uid;
}

gid_t get_orig_gid(void)
{
  return gid;
}

int priv_drop(void)
{
  assert(PRIVS_ARE_OFF);
  if (skip_priv_setting)
    return 1;
  /* We set the same values as they are now.
   * The trick is that if the first arg != -1 then saved-euid is reset.
   * This allows to avoid the use of non-standard setresuid(). */
  if (setreuid(uid, cur_euid) || setregid(gid, cur_egid)) {
    error("Cannot drop root uid or gid!\n");
    return 0;
  }
  /* Now check that saved-euids are actually reset: privs should fail. */
  if (seteuid(euid) == 0 || setegid(egid) == 0) {
    error("privs were not dropped\n");
    leavedos(3);
    return 0;
  }
  skip_priv_setting = 1;
  if (uid) can_do_root_stuff = 0;
  return 1;
}

void priv_init(void)
{
  const char *sh = getenv("SUDO_HOME"); // theoretical future var
  const char *h = getenv("HOME");
  uid = getuid();
  /* suid bit only sets euid & suid but not uid, sudo sets all 3 */
  if (!uid) under_root_login = 1;
  euid = cur_euid = geteuid();
  if (!euid) can_do_root_stuff = 1;
  if (!uid && !euid) skip_priv_setting = 1;
  gid = getgid();
  egid = cur_egid = getegid();

  /* must store the /proc/self/exe symlink contents before dropping
     privs! */
  dosemu_proc_self_exe = readlink_malloc("/proc/self/exe");
  /* For Fedora we must also save a file descriptor to /proc/self/maps */
  dosemu_proc_self_maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (!sh)
    sh = getenv("DOSEMU_SUDO_HOME");
  /* see if -E was used */
  if (under_root_login && sh && h && strcmp(sh, h) == 0) {
    /* check for sudo and set to original user */
    char *s = getenv("SUDO_GID");
    if (s) {
      gid = atoi(s);
      if (gid) {
        setregid(gid, egid);
      }
    }
    s = getenv("SUDO_UID");
    if (s) {
      uid = atoi(s);
      if (uid) {
        skip_priv_setting = under_root_login = 0;
	using_sudo = 1;
        setreuid(uid, euid);
      }
    }
  }

  if (!can_do_root_stuff)
    skip_priv_setting = 1;

  if (!skip_priv_setting) _priv_off();
}
