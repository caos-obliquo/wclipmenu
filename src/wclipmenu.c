/*
 * wclipmenu — wmenu-based clipboard picker over kaprica (Wayland).
 *
 * Phase 2: text picker only. Image picker (+ sixel previews) comes later.
 *
 * Runs on demand from a keybind and exits after one selection:
 *   wclipmenu            -> pick from kaprica text history via wmenu, copy choice
 *   wclipmenu list       -> print snippet lines to stdout (debug/CLI path)
 *   wclipmenu copy <id>  -> kapc copy -i <id> (debug/CLI path)
 *
 * Zero resident footprint: no daemon, no polling, no config files.
 * Depends only on libc + external wmenu + kaprica CLI (kapd/kapc).
 *
 * Env overrides (for testing):
 *   WCLIPMENU_WMENU  path to wmenu binary (default: wmenu from PATH)
 *   WCLIPMENU_LIMIT  max entries from kaprica (default: 100)
 *   WCLIPMENU_DB     db file path passed to kapc as -D (default: kaprica's)
 */
#define _DEFAULT_SOURCE /* usleep-free; keeps PATH_MAX, etc. before any header */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define KAPC_PATH "/usr/local/bin/kapc"
#define DEFAULT_LIMIT 100
#define WMENU_LINES 15
#define WMENU_PROMPT "clipboard:"

struct entry {
	long id;
	const char *snippet; /* pointer into the search buffer */
};

/* ---- small spawn helpers ------------------------------------------- */

/*
 * Run argv with no input; capture stdout into *out (NUL-terminated).
 * Returns child exit status, or -1 on spawn failure. *out is malloc'd,
 * caller frees. Empty on error is possible only if spawn failed.
 */
static int run_capture(const char *const argv[], char **out)
{
	int pipefd[2];
	pid_t pid;
	int status;

	if (pipe(pipefd) == -1)
		return -1;
	pid = fork();
	if (pid == -1) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1)
			_exit(127);
		close(pipefd[1]);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pipefd[1]);

	/* read all output */
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		return -1;
	}
	for (;;) {
		if (len + 1 >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(pipefd[0]);
				waitpid(pid, &status, 0);
				return -1;
			}
			buf = nb;
		}
		ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(pipefd[0]);
			waitpid(pid, &status, 0);
			return -1;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	buf[len] = '\0';
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) == -1)
		status = 0;
	*out = buf;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * Run argv in a child and return immediately (do NOT wait).
 * Used for `kapc copy`: kapc daemonizes a clipboard-owner grandchild
 * that must outlive us; waiting would hang until that owner dies.
 */
static int spawn_detach(const char *const argv[])
{
	pid_t pid = fork();
	if (pid == -1)
		return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	return 0;
}

/* ---- kaprica plumbing ---------------------------------------------- */

/* `kapc search -t text/plain -L -l <n> [-D <db>]` -> raw stdout in *out. */
static int kapc_search(int limit, char **out)
{
	char lim[16];
	snprintf(lim, sizeof lim, "%d", limit);
	const char *db = getenv("WCLIPMENU_DB");
	if (db && *db) {
		const char *argv[] = { KAPC_PATH, "search", "-t", "text/plain",
				       "-L", "-l", lim, "-D", db, NULL };
		return run_capture(argv, out);
	}
	const char *argv[] = { KAPC_PATH, "search", "-t", "text/plain",
			       "-L", "-l", lim, NULL };
	return run_capture(argv, out);
}

static char *kapc_copy_cmd(long id)
{
	char *cmd = malloc(32);
	if (!cmd)
		return NULL;
	snprintf(cmd, 32, "%ld", id);
	return cmd;
}

/*
 * Parse `<id>\t<snippet>` lines from `kapc search -L` output into es[].
 * Snippets point into raw (mutated in place). Tabs inside a snippet are
 * replaced with spaces for single-line wmenu display. Lines that do not
 * start with digits immediately followed by a tab are skipped — this keeps
 * multi-line snippets from misaligning the parse (their continuation lines
 * carry no leading id).
 */
static size_t parse_entries(char *raw, struct entry *es, size_t max)
{
	size_t n = 0;
	char *line = raw;

	while (line && n < max) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		char *tab = strchr(line, '\t');
		if (!tab) {
			if (!nl)
				break;
			line = nl + 1;
			continue;
		}
		char *end = NULL;
		long id = strtol(line, &end, 10);
		if (end != tab || end == line) {
			if (!nl)
				break;
			line = nl + 1;
			continue;
		}

		char *s = tab + 1;
		size_t ll = strlen(s);
		while (ll > 0 && s[ll - 1] == '\r')
			s[--ll] = '\0';
		for (char *p = s; *p; p++) {
			if (*p == '\t')
				*p = ' ';
		}

		es[n].id = id;
		es[n].snippet = s;
		n++;
		if (!nl)
			break;
		line = nl + 1;
	}
	return n;
}

/* ---- wmenu --------------------------------------------------------- */

/*
 * Feed snippets to wmenu, read the chosen line back.
 * Returns 0 and writes selection (NUL-terminated, malloc'd) on success,
 * -1 if wmenu failed or the user cancelled.
 */
static int run_wmenu(const struct entry *es, size_t n, char **sel)
{
	const char *wmenu = getenv("WCLIPMENU_WMENU");
	if (!wmenu || !*wmenu)
		wmenu = "wmenu";

	int inpipe[2], outpipe[2];
	pid_t pid;
	int status;

	if (pipe(inpipe) == -1 || pipe(outpipe) == -1) {
		if (inpipe[0] >= 0) {
			close(inpipe[0]);
			close(inpipe[1]);
		}
		return -1;
	}
	pid = fork();
	if (pid == -1) {
		close(inpipe[0]);
		close(inpipe[1]);
		close(outpipe[0]);
		close(outpipe[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(inpipe[0], STDIN_FILENO);
		dup2(outpipe[1], STDOUT_FILENO);
		close(inpipe[0]);
		close(inpipe[1]);
		close(outpipe[0]);
		close(outpipe[1]);
		char lines[16];
		snprintf(lines, sizeof lines, "%d", WMENU_LINES);
		const char *argv[] = { wmenu, "-l", lines, "-i", "-p",
				       WMENU_PROMPT, NULL };
		execv(wmenu, (char *const *)argv);
		/* fall back to PATH lookup for a bare name */
		if (strchr(wmenu, '/') == NULL) {
			execvp(wmenu, (char *const *)argv);
		}
		_exit(127);
	}
	close(inpipe[0]);
	close(outpipe[1]);

	/* feed snippets */
	size_t i;
	for (i = 0; i < n; i++) {
		size_t sl = strlen(es[i].snippet);
		if (write(inpipe[1], es[i].snippet, sl) != (ssize_t)sl ||
		    write(inpipe[1], "\n", 1) != 1)
			break;
	}
	close(inpipe[1]);

	/* read selection */
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(outpipe[0]);
		waitpid(pid, &status, 0);
		return -1;
	}
	for (;;) {
		if (len + 1 >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(outpipe[0]);
				waitpid(pid, &status, 0);
				return -1;
			}
			buf = nb;
		}
		ssize_t r = read(outpipe[0], buf + len, cap - len - 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(outpipe[0]);
			waitpid(pid, &status, 0);
			return -1;
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	buf[len] = '\0';
	close(outpipe[0]);
	if (waitpid(pid, &status, 0) == -1)
		status = 0;

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(buf);
		return -1; /* cancelled or crashed */
	}
	/* trim trailing newline */
	size_t bl = strlen(buf);
	while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r'))
		buf[--bl] = '\0';
	if (bl == 0) {
		free(buf);
		return -1;
	}
	*sel = buf;
	return 0;
}

/* ---- subcommands ---------------------------------------------------- */

static int cmd_list(int limit)
{
	char *raw = NULL;
	int st = kapc_search(limit, &raw);
	if (st != 0 || !raw) {
		fprintf(stderr, "wclipmenu: kapc search failed (status %d)\n", st);
		free(raw);
		return 1;
	}
	struct entry *es = calloc((size_t)limit, sizeof *es);
	if (!es) {
		free(raw);
		return 1;
	}
	size_t n = parse_entries(raw, es, (size_t)limit);
	size_t i;
	for (i = 0; i < n; i++)
		printf("%s\n", es[i].snippet);
	free(es);
	free(raw);
	return 0;
}

static int cmd_copy(long id)
{
	char *ids = kapc_copy_cmd(id);
	if (!ids)
		return 1;
	const char *db = getenv("WCLIPMENU_DB");
	int st;
	if (db && *db) {
		const char *argv[] = { KAPC_PATH, "copy", "-i", ids, "-D", db,
				       NULL };
		st = spawn_detach(argv);
	} else {
		const char *argv[] = { KAPC_PATH, "copy", "-i", ids, NULL };
		st = spawn_detach(argv);
	}
	if (st == -1) {
		fprintf(stderr, "wclipmenu: failed to spawn kapc copy\n");
		free(ids);
		return 1;
	}
	free(ids);
	return 0;
}

static int cmd_pick(int limit)
{
	char *raw = NULL;
	int st = kapc_search(limit, &raw);
	if (st != 0 || !raw) {
		fprintf(stderr, "wclipmenu: kapc search failed (status %d)\n", st);
		free(raw);
		return 1;
	}
	struct entry *es = calloc((size_t)limit, sizeof *es);
	if (!es) {
		free(raw);
		return 1;
	}
	size_t n = parse_entries(raw, es, (size_t)limit);
	if (n == 0) {
		fprintf(stderr, "wclipmenu: empty clipboard history\n");
		free(es);
		free(raw);
		return 1;
	}

	char *sel = NULL;
	if (run_wmenu(es, n, &sel) == -1) {
		free(es);
		free(raw);
		return 0; /* user cancelled — not an error */
	}

	/* map selection back to its id */
	size_t i;
	long id = -1;
	for (i = 0; i < n; i++) {
		if (strcmp(es[i].snippet, sel) == 0) {
			id = es[i].id;
			break;
		}
	}
	free(sel);
	if (id < 0) {
		fprintf(stderr, "wclipmenu: selection not found in history\n");
		free(es);
		free(raw);
		return 1;
	}
	free(es);
	free(raw);
	return cmd_copy(id);
}

static int parse_limit(const char *s)
{
	char *end = NULL;
	long v = strtol(s, &end, 10);
	if (end == s || v < 1 || v > 10000)
		return -1;
	return (int)v;
}

int main(int argc, char **argv)
{
	int limit = DEFAULT_LIMIT;
	const char *envlim = getenv("WCLIPMENU_LIMIT");
	if (envlim && *envlim) {
		int v = parse_limit(envlim);
		if (v < 0) {
			fprintf(stderr, "wclipmenu: bad WCLIPMENU_LIMIT\n");
			return 1;
		}
		limit = v;
	}

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		if (argc >= 4 && strcmp(argv[2], "-n") == 0) {
			int v = parse_limit(argv[3]);
			if (v < 0) {
				fprintf(stderr, "usage: wclipmenu list [-n count]\n");
				return 1;
			}
			limit = v;
		}
		return cmd_list(limit);
	}
	if (argc >= 2 && strcmp(argv[1], "copy") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: wclipmenu copy <id>\n");
			return 1;
		}
		char *end = NULL;
		long id = strtol(argv[2], &end, 10);
		if (end == argv[2] || *end != '\0') {
			fprintf(stderr, "wclipmenu: bad id '%s'\n", argv[2]);
			return 1;
		}
		return cmd_copy(id);
	}
	if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		printf("usage: wclipmenu [list [-n count] | copy <id>]\n");
		return 0;
	}
	if (argc > 1) {
		fprintf(stderr, "usage: wclipmenu [list [-n count] | copy <id>]\n");
		return 1;
	}
	return cmd_pick(limit);
}
