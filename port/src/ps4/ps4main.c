// PS4 process entry point: sets up the data directory and an early boot log, then runs the
// game's own main() (renamed to pdMain on this platform) on a thread with a large stack.

#include "platform.h"

#ifdef PLATFORM_PS4

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/SystemService.h>

#include "ps4platform.h"
#include "versioninfo.h"

extern int pdMain(int argc, const char **argv);
extern void ps4HeapGetStats(size_t *arena, size_t *inuse, size_t *peak);

// The decompiled game code recurses quite a bit; the default main thread stack is tight.
#define PS4_GAME_STACK_SIZE (8 * 1024 * 1024)
// stdout/stderr go here from the first instruction on, so failures before the game's own
// log is open (Piglet setup in particular) still leave a trace. Piglet's own [PIG] messages
// end up here as well.
#define PS4_BOOT_LOG PS4_DATA_DIR "/ps4_boot.log"

static int bootLogFd = -1;

static void ps4BootLogRaw(const char *text)
{
	if (bootLogFd >= 0) {
		write(bootLogFd, text, strlen(text));
		fsync(bootLogFd);
	}
}

static void ps4FatalSignal(int sig, siginfo_t *info, void *ctx)
{
	(void)ctx;
	char line[160];
	snprintf(line, sizeof(line), "PS4: FATAL: signal %d, fault address %p\n", sig, info ? info->si_addr : NULL);
	ps4BootLogRaw(line);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void ps4InstallBootLog(void)
{
	bootLogFd = open(PS4_BOOT_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (bootLogFd >= 0) {
		// same descriptor for both, so lines stay in order
		dup2(bootLogFd, STDOUT_FILENO);
		dup2(bootLogFd, STDERR_FILENO);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	// the OpenOrbis header misspells its sa_sigaction macro; go through the union
	act.__sa_handler.__sa_sigaction = ps4FatalSignal;
	act.sa_flags = SA_SIGINFO;
	const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	for (size_t i = 0; i < sizeof(sigs) / sizeof(*sigs); ++i) {
		sigaction(sigs[i], &act, NULL);
	}
}

// With the stock OpenOrbis headers stat() reported garbage sizes (see compat/ps4_fixups.h in the
// build scripts). Compare stat() and lseek() on a file that is always there.
static void ps4CheckFileSizes(void)
{
	const char *path = "/app0/eboot.bin";
	struct stat st;
	memset(&st, 0, sizeof(st));
	const int sret = stat(path, &st);
	long long seek = -1;
	const int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		seek = (long long)lseek(fd, 0, SEEK_END);
		close(fd);
	}
	char line[200];
	snprintf(line, sizeof(line), "PS4: file size check on %s: stat %s, st_size %lld, lseek %lld -> %s\n", path,
		sret == 0 ? "ok" : "failed", (long long)st.st_size, seek,
		(sret == 0 && (long long)st.st_size == seek) ? "consistent" : "MISMATCH");
	ps4BootLogRaw(line);
}

static void ps4ReturnToSystem(void)
{
	size_t arena = 0, inuse = 0, peak = 0;
	ps4HeapGetStats(&arena, &inuse, &peak);
	printf("PS4: heap arena %zu MiB, in use %zu MiB, peak %zu MiB\n", arena >> 20, inuse >> 20, peak >> 20);
	fflush(stdout);
	// the orderly way back to the home screen
	sceSystemServiceLoadExec("exit", NULL);
}

#define PS4_ROM_NAME "pd." VERSION_ROMID ".z64"
// where earlier builds expected the ROM
#define PS4_OLD_BASE_DIR PS4_DATA_DIR "/data"

// The ROM (and pd.gbc) go straight into /data/perfectdark, next to pd.ini and the saves. Installs
// that still have the ROM in the old data/ subfolder keep working.
static const char *ps4ChooseBaseDir(void)
{
	if (access(PS4_DATA_DIR "/" PS4_ROM_NAME, F_OK) != 0 && access(PS4_OLD_BASE_DIR "/" PS4_ROM_NAME, F_OK) == 0) {
		ps4BootLogRaw("PS4: ROM found in the old location " PS4_OLD_BASE_DIR "/, using that. "
			"Move it to " PS4_DATA_DIR "/ when convenient.\n");
		return PS4_OLD_BASE_DIR;
	}
	return PS4_DATA_DIR;
}

static void *ps4GameThread(void *arg)
{
	(void)arg;
	ps4BootLogRaw("PS4: game thread started\n");

	// Relative paths fail with EINVAL rather than ENOENT on PS4, so every location the game
	// probes is given explicitly.
	const char *argv[] = {
		"/app0/eboot.bin",
		"--log",
		"--basedir", ps4ChooseBaseDir(),
		"--savedir", PS4_DATA_DIR,
		NULL
	};
	const int argc = (int)(sizeof(argv) / sizeof(*argv)) - 1;

	pdMain(argc, argv);

	ps4BootLogRaw("PS4: pdMain returned\n");
	exit(0);
	return NULL;
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	if (mkdir(PS4_DATA_DIR, 0777) != 0 && errno != EEXIST) {
		printf("PS4: could not create " PS4_DATA_DIR ": %s\n", strerror(errno));
	}
	if (chdir(PS4_DATA_DIR) != 0) {
		printf("PS4: could not chdir to " PS4_DATA_DIR ": %s\n", strerror(errno));
	}

	ps4InstallBootLog();
	ps4BootLogRaw("PS4: Perfect Dark boot\n");
	ps4CheckFileSizes();

	atexit(ps4ReturnToSystem);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PS4_GAME_STACK_SIZE);

	pthread_t thr;
	const int ret = pthread_create(&thr, &attr, ps4GameThread, NULL);
	pthread_attr_destroy(&attr);

	if (ret != 0) {
		printf("PS4: could not create the game thread (%d), running on the main thread\n", ret);
		ps4GameThread(NULL);
	} else {
		pthread_join(thr, NULL);
	}

	return 0;
}

#endif // PLATFORM_PS4
