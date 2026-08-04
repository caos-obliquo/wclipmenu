/*
 * wclipmenu — wmenu-based clipboard picker over kaprica (Wayland).
 *
 * Phase 2: text picker. Phase 3: image picker with wmenu PNG thumbnails.
 *
 * Runs on demand from a keybind and exits after one selection:
 *   wclipmenu            -> pick from kaprica text history via wmenu, copy choice
 *   wclipmenu list       -> print snippet lines to stdout (debug/CLI path)
 *   wclipmenu copy <id>  -> kapc copy -i <id> (debug/CLI path)
 *   wclipmenu image      -> pick from kaprica image/png history via wmenu
 *                           (PNG thumbnails), copy choice
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
#define DEFAULT_IMAGE_LIMIT 10
#define WMENU_LINES 15
#define WMENU_PROMPT "clipboard:"

struct entry {
	long id;
	const char *snippet; /* pointer into the search buffer */
};

/* ---- small spawn helpers ------------------------------------------- */

/*
 * Run argv with no input; capture stdout into *out (malloc'd) with its
 * length in *outlen. Returns child exit status, or -1 on spawn failure.
 * The buffer is not NUL-terminated; use run_capture for string output.
 */
static int run_capture_raw(const char *const argv[], char **out, size_t *outlen)
{
	int pipefd[2];
	pid_t pid;
	int status;

	*out = NULL;
	*outlen = 0;
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
	*outlen = len;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * String variant of run_capture_raw: capture stdout into *out
 * (NUL-terminated). Returns child exit status, or -1 on spawn failure.
 * *out is malloc'd, caller frees.
 */
static int run_capture(const char *const argv[], char **out)
{
	size_t len;
	return run_capture_raw(argv, out, &len);
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

/*
 * Run argv and wait for it, returning the child exit status (or -1 on
 * spawn failure). The child's stderr is discarded — used for magick, where
 * a corrupt source makes it complain about the image header on every run.
 */
static int run_quiet(const char *const argv[])
{
	pid_t pid = fork();
	if (pid == -1)
		return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	int status;
	if (waitpid(pid, &status, 0) == -1)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ---- kaprica plumbing ---------------------------------------------- */

/* `kapc search -t <type> -L -l <n> [-D <db>]` -> raw stdout in *out. */
static int kapc_search_type(const char *type, int limit, char **out)
{
	char lim[16];
	snprintf(lim, sizeof lim, "%d", limit);
	const char *db = getenv("WCLIPMENU_DB");
	if (db && *db) {
		const char *argv[] = { KAPC_PATH, "search", "-t", type,
				       "-L", "-l", lim, "-D", db, NULL };
		return run_capture(argv, out);
	}
	const char *argv[] = { KAPC_PATH, "search", "-t", type,
			       "-L", "-l", lim, NULL };
	return run_capture(argv, out);
}

static int kapc_search(int limit, char **out)
{
	return kapc_search_type("text/plain", limit, out);
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
 * Feed raw preformatted lines (NUL-terminated) to wmenu, read the chosen
 * line back. Returns 0 and writes selection (NUL-terminated, malloc'd) on
 * success, -1 if wmenu failed or the user cancelled.
 */
static int run_wmenu_lines(const char *input, char **sel)
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
/* -N normal background alpha from RRGGBBAA (cc = minimal transparency) */
		const char *argv[] = { wmenu, "-l", lines, "-i", "-c", "-p",
				       WMENU_PROMPT, "-N", "#222222cc",
				       "-S", "#3b3b3bf2", "-s", "#ffffff", NULL };
		execv(wmenu, (char *const *)argv);
		/* fall back to PATH lookup for a bare name */
		if (strchr(wmenu, '/') == NULL) {
			execvp(wmenu, (char *const *)argv);
		}
		_exit(127);
	}
	close(inpipe[0]);
	close(outpipe[1]);

	/* feed input lines */
	size_t len = strlen(input);
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(inpipe[1], input + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}
	close(inpipe[1]);

	/* read selection */
	size_t cap = 4096, len2 = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(outpipe[0]);
		waitpid(pid, &status, 0);
		return -1;
	}
	for (;;) {
		if (len2 + 1 >= cap) {
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
		ssize_t r = read(outpipe[0], buf + len2, cap - len2 - 1);
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
		len2 += (size_t)r;
	}
	buf[len2] = '\0';
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

/*
 * Feed snippets to wmenu, read the chosen line back.
 * Returns 0 and writes selection (NUL-terminated, malloc'd) on success,
 * -1 if wmenu failed or the user cancelled.
 */
static int run_wmenu(const struct entry *es, size_t n, char **sel)
{
	size_t total = 0;
	size_t i;
	for (i = 0; i < n; i++)
		total += strlen(es[i].snippet) + 1;
	char *input = malloc(total + 1);
	if (!input)
		return -1;
	size_t off = 0;
	for (i = 0; i < n; i++) {
		size_t sl = strlen(es[i].snippet);
		memcpy(input + off, es[i].snippet, sl);
		off += sl;
		input[off++] = '\n';
	}
	input[off] = '\0';
	int st = run_wmenu_lines(input, sel);
	free(input);
	return st;
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

struct pick {
	char thumb[64];
	size_t es_idx; /* index into es[] the thumbnail was built from */
};

/*
 * Locate the magick binary via the shell and stash its path in *path.
 * Returns 0 on success, -1 if not found (caller errors out: thumbnails
 * are the whole point of the image picker).
 */
static int resolve_magick(char **path)
{
	char *out = NULL;
	const char *argv[] = { "/bin/sh", "-c", "command -v magick 2>/dev/null",
			       NULL };
	if (run_capture(argv, &out) != 0 || !out) {
		free(out);
		return -1;
	}
	size_t l = strlen(out);
	while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r'))
		out[--l] = '\0';
	if (l == 0) {
		free(out);
		return -1;
	}
	*path = out;
	return 0;
}

/*
 * Render a 96x96 PNG thumbnail for entry id into *thumb. Two-step file
 * pipeline keeps the child plumbing trivial: `kapc paste` bytes land in an
 * mkstemp raw file, then `magick <raw> -resize 96x96 <thumb>` resizes it.
 * Returns 0 with thumb filled in, -1 on any failure (raw file and any
 * partial output are unlinked; caller falls back to a plain text row).
 */
static int render_thumb(const char *magick, long id, char *thumb,
			size_t thumbsz)
{
	char ids[32];
	snprintf(ids, sizeof ids, "%ld", id);
	const char *db = getenv("WCLIPMENU_DB");

	/* cache hit: a thumb from a prior launch is reusable - skip the
	 * expensive kapc paste + magick pipeline entirely (image picker
	 * must open near-instantly on repeat use) */
	snprintf(thumb, thumbsz, "/tmp/wclipmenu-thumb-%ld.png", id);
	if (access(thumb, R_OK) == 0)
		return 0;

	char rawtmpl[] = "/tmp/wclipmenu-raw-XXXXXX";
	int fd = mkstemp(rawtmpl);
	if (fd == -1)
		return -1;

	char *paste = NULL;
	size_t plen = 0;
	int st;
	if (db && *db) {
		const char *argv[] = { KAPC_PATH, "paste", "-i", ids, "-D", db,
				       NULL };
		st = run_capture_raw(argv, &paste, &plen);
	} else {
		const char *argv[] = { KAPC_PATH, "paste", "-i", ids, NULL };
		st = run_capture_raw(argv, &paste, &plen);
	}
	if (st != 0 || !paste) {
		close(fd);
		unlink(rawtmpl);
		free(paste);
		return -1;
	}
	size_t off = 0;
	while (off < plen) {
		ssize_t w = write(fd, paste + off, plen - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}
	free(paste);
	if (off != plen) {
		close(fd);
		unlink(rawtmpl);
		return -1;
	}
	close(fd);

	/* .png suffix keeps magick's read policy deterministic (mkstemp's
	 * XXXXXX leaves no extension to sniff) */
	char rawin[sizeof rawtmpl + 4];
	snprintf(rawin, sizeof rawin, "%s.png", rawtmpl);
	if (rename(rawtmpl, rawin) == -1) {
		unlink(rawtmpl);
		return -1;
	}

	/* wmenu's cairo_image_surface_create_from_png needs the .png suffix */
	snprintf(thumb, thumbsz, "/tmp/wclipmenu-thumb-%ld.png", id);
	unlink(thumb); /* drop any stale partial from a prior run */
	const char *argv[] = { magick, rawin, "-resize", "96x96", thumb, NULL };
	st = run_quiet(argv);
	unlink(rawin);
	if (st != 0 || access(thumb, F_OK) == -1) {
		unlink(thumb); /* partial output must not survive */
		return -1;
	}
	return 0;
}

static int cmd_image(int limit)
{
	char *magick = NULL;
	if (resolve_magick(&magick) == -1) {
		fprintf(stderr, "wclipmenu: magick not found "
				"(required for image picker)\n");
		return 1;
	}

	char *raw = NULL;
	int st = kapc_search_type("image/png", limit, &raw);
	if (st != 0 || !raw) {
		fprintf(stderr, "wclipmenu: kapc search failed (status %d)\n", st);
		free(raw);
		free(magick);
		return 1;
	}
	struct entry *es = calloc((size_t)limit, sizeof *es);
	if (!es) {
		free(raw);
		free(magick);
		return 1;
	}
	size_t n = parse_entries(raw, es, (size_t)limit);
	if (n == 0) {
		fprintf(stderr, "wclipmenu: empty clipboard history\n");
		free(es);
		free(raw);
		free(magick);
		return 1;
	}

	/* render a thumbnail per entry; failed renders fall back to a plain
	 * text row so the snippet stays visible and pickable */
	struct pick *picks = calloc(n, sizeof *picks);
	if (!picks) {
		free(es);
		free(raw);
		free(magick);
		return 1;
	}
	size_t i;
	/* parallel thumbnail render: kapc paste + magick dominate first-open
	 * time; fork one child per entry so N thumbs build concurrently and
	 * the picker opens near-instantly even on a cold cache */
	size_t nlive = 0;
	for (i = 0; i < n; i++) {
		pid_t pid = fork();
		if (pid == 0) {
			char tbuf[64];
			int rc = render_thumb(magick, es[i].id, tbuf,
					      sizeof tbuf);
			_exit(rc == 0 ? 0 : 1);
		} else if (pid > 0) {
			nlive++;
		}
	}
	while (nlive > 0) {
		waitpid(-1, NULL, 0);
		nlive--;
	}
	for (i = 0; i < n; i++) {
		picks[i].es_idx = i;
		snprintf(picks[i].thumb, sizeof picks[i].thumb,
			 "/tmp/wclipmenu-thumb-%ld.png", es[i].id);
		if (access(picks[i].thumb, R_OK) != 0)
			picks[i].thumb[0] = '\0'; /* no thumbnail, plain row */
	}

	/* wmenu input: [img:<thumbpath>]<snippet> for thumbnailed rows, plain
	 * <snippet> for the rest */
	size_t cap = 4096, len = 0;
	char *lines = malloc(cap);
	if (!lines) {
		free(picks);
		free(es);
		free(raw);
		free(magick);
		return 1;
	}
	for (i = 0; i < n; i++) {
		const char *snippet = es[picks[i].es_idx].snippet;
		size_t pre = picks[i].thumb[0] ? strlen("[img:") +
			      strlen(picks[i].thumb) + 1 : 0;
		size_t need = pre + strlen(snippet) + 2;
		if (len + need >= cap) {
			while (cap < len + need + 1)
				cap *= 2;
			char *nb = realloc(lines, cap);
			if (!nb) {
				free(lines);
				free(picks);
				free(es);
				free(raw);
				free(magick);
				return 1;
			}
			lines = nb;
		}
		int m = picks[i].thumb[0]
			? snprintf(lines + len, cap - len, "[img:%s]%s\n",
				   picks[i].thumb, snippet)
			: snprintf(lines + len, cap - len, "%s\n", snippet);
		if (m < 0 || (size_t)m >= cap - len) {
			free(lines);
			free(picks);
			free(es);
			free(raw);
			free(magick);
			return 1;
		}
		len += (size_t)m;
	}

	char *sel = NULL;
	if (run_wmenu_lines(lines, &sel) == -1) {
		free(lines);
		free(picks);
		free(es);
		free(raw);
		free(magick);
		return 0; /* user cancelled — not an error */
	}
	free(lines);

	/* thumbnailed rows: "[img:<thumbpath>]<snippet>", id in the thumb
	 * filename (/tmp/wclipmenu-thumb-<id>.png). Plain fallback rows are
	 * the bare snippet, matched by text like cmd_pick. */
	long id = -1;
	if (strncmp(sel, "[img:", 5) == 0) {
		const char *close = strchr(sel + 5, ']');
		const char *marker = close ? strstr(sel + 5, "wclipmenu-thumb-")
					   : NULL;
		if (marker) {
			char *end = NULL;
			long v = strtol(marker + strlen("wclipmenu-thumb-"),
					&end, 10);
			if (end != marker + strlen("wclipmenu-thumb-"))
				id = v;
		}
	} else {
		for (i = 0; i < n; i++) {
			if (strcmp(es[i].snippet, sel) == 0) {
				id = es[i].id;
				break;
			}
		}
	}
	free(sel);
	free(picks);
	free(es);
	free(raw);
	free(magick);
	if (id < 0) {
		fprintf(stderr, "wclipmenu: selection not found in history\n");
		return 1;
	}
	return cmd_copy(id);
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
	if (argc >= 2 && strcmp(argv[1], "image") == 0) {
		if (argc >= 4 && strcmp(argv[2], "-n") == 0) {
			int v = parse_limit(argv[3]);
			if (v < 0) {
				fprintf(stderr, "usage: wclipmenu image [-n count]\n");
				return 1;
			}
			limit = v;
		}
		return cmd_image(limit);
	}
	if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		printf("usage: wclipmenu [list [-n count] | copy <id> | image]\n");
		return 0;
	}
	if (argc > 1) {
		fprintf(stderr, "usage: wclipmenu [list [-n count] | copy <id> | image]\n");
		return 1;
	}
	return cmd_pick(limit);
}
