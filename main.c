/*
 * Copyright 2005-2020 Gentoo Foundation
 * Distributed under the terms of the GNU General Public License v2
 *
 * Copyright 2005-2008 Ned Ludd        - <solar@gentoo.org>
 * Copyright 2005-2014 Mike Frysinger  - <vapier@gentoo.org>
 * Copyright 2018-     Fabian Groffen  - <grobian@gentoo.org>
 */

#include "main.h"
#include "applets.h"

#include <xalloc.h>
#include <assert.h>
#include <ctype.h>
#include <sys/time.h>
#include <limits.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "eat_file.h"
#include "rmspace.h"
#include "scandirat.h"
#include "set.h"
#include "xasprintf.h"

/* variables to control runtime behavior */
char *main_overlay;
char *module_name = NULL;
int verbose = 0;
int quiet = 0;
int twidth;
char pretend = 0;
char *portarch;
char *portroot;
char *config_protect;
char *config_protect_mask;
char *portvdb;
char *portlogdir;
char *pkg_install_mask;
char *binhost;
char *pkgdir;
char *port_tmpdir;
char *features;
char *install_mask;
DECLARE_ARRAY(overlays);
DECLARE_ARRAY(overlay_names);
DECLARE_ARRAY(overlay_src);

static char *portedb;
static char *eprefix;
static char *accept_license;

#define STR_DEFAULT "built-in default"

/* helper functions for showing errors */
const char *argv0;

FILE *warnout;

#ifdef EBUG
# include <sys/resource.h>
static void
init_coredumps(void)
{
	struct rlimit rl;
	rl.rlim_cur = RLIM_INFINITY;
	rl.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &rl);
}
#endif

void
no_colors(void)
{
	BOLD = NORM = BLUE = DKBLUE = CYAN = GREEN = DKGREEN = \
		   MAGENTA = RED = YELLOW = BRYELLOW = WHITE = "";
	setenv("NOCOLOR", "true", 1);
}

void
setup_quiet(void)
{
	/* "e" for FD_CLOEXEC */
	if (quiet == 0)
		warnout = fopen("/dev/null", "we");
	++quiet;
}

/* display usage and exit */
void
usage(int status, const char *flags, struct option const opts[],
      const char * const help[], const char *desc, int blabber)
{
	const char opt_arg[] = "[arg]";
	const char a_arg[] = "<arg>";
	size_t a_arg_len = strlen(a_arg) + 1;
	size_t i;
	size_t optlen;
	size_t l;
	size_t prefixlen;
	const char *hstr;
	FILE *fp = status == EXIT_SUCCESS ? stdout : warnout;

	if (blabber == 0) {
		fprintf(fp, "%susage:%s %sq%s %s<applet> <args>%s  : %s"
			"invoke a portage utility applet\n\n", GREEN,
			NORM, YELLOW, NORM, DKBLUE, RED, NORM);
		fprintf(fp, "%scurrently defined applets:%s\n", GREEN, NORM);
		for (i = 0; applets[i].desc; ++i)
			if (applets[i].func)
				fprintf(fp, " %s%9s%s %s%-16s%s%s:%s %s\n",
					YELLOW, applets[i].name, NORM,
					DKBLUE, applets[i].opts, NORM,
					RED, NORM, _(applets[i].desc));
	} else if (blabber > 0) {
		fprintf(fp, "%susage:%s %s%s%s [opts] %s%s%s %s:%s %s\n",
			GREEN, NORM,
			YELLOW, applets[blabber].name, NORM,
			DKBLUE, applets[blabber].opts, NORM,
			RED, NORM, _(applets[blabber].desc));
		if (desc)
			fprintf(fp, "\n%s\n", desc);
	}
	if (module_name != NULL)
		fprintf(fp, "%sloaded module:%s\n%s%8s%s %s<args>%s\n",
			GREEN, NORM, YELLOW, module_name, NORM, DKBLUE, NORM);

	/* Prescan the --long opt length to auto-align. */
	optlen = 0;
	for (i = 0; opts[i].name; ++i) {
		l = strlen(opts[i].name);
		if (opts[i].has_arg != no_argument)
			l += a_arg_len;
		optlen = MAX(l, optlen);
	}

	fprintf(fp, "\n%soptions:%s -[%s]\n", GREEN, NORM, flags);
	for (i = 0; opts[i].name; ++i) {
		/* this assert is a life saver when adding new applets. */
		assert(help[i] != NULL);

		/* first output the short flag if it has one */
		if (opts[i].val > '~' || opts[i].val < ' ')
			fprintf(fp, "      ");
		else
			fprintf(fp, "  -%c, ", opts[i].val);

		/* then the long flag */
		if (opts[i].has_arg == no_argument)
			fprintf(fp, "--%-*s %s*%s ", (int)optlen, opts[i].name,
				RED, NORM);
		else
			fprintf(fp, "--%s %s%s%s%*s %s*%s ",
				opts[i].name,
				DKBLUE, (opts[i].has_arg == a_argument ? a_arg : opt_arg), NORM,
				(int)(optlen - strlen(opts[i].name) - a_arg_len), "",
				RED, NORM);

		/* then wrap the help text, if necessary */
		prefixlen = 6 + 2 + optlen + 1 + 1 + 1;
		if ((size_t)twidth < prefixlen + 10) {
			fprintf(fp, "%s\n", _(help[i]));
		} else {
			const char *t;
			hstr = _(help[i]);
			l = strlen(hstr);
			while (twidth - prefixlen < l) {
				/* search backwards for a space */
				t = &hstr[twidth - prefixlen];
				while (t > hstr && !isspace((int)*t))
					t--;
				if (t == hstr)
					break;
				fprintf(fp, "%.*s\n%*s",
						(int)(t - hstr), hstr, (int)prefixlen, "");
				l -= t + 1 - hstr;
				hstr = t + 1;  /* skip space */
			}
			fprintf(fp, "%s\n", hstr);
		}
	}
	exit(status);
}

void
version_barf(void)
{
	const char *vcsid = "";
	const char *eprefixid = "";

#ifndef VERSION
# define VERSION "git"
#endif

#ifdef VCSID
	vcsid = " (" VCSID ")";
#endif

	if (strlen(CONFIG_EPREFIX) > 1)
		eprefixid = "configured for " CONFIG_EPREFIX "\n";

	printf("portage-utils-%s%s\n"
	       "%s"
	       "written for Gentoo by solar, vapier and grobian\n",
	       VERSION, vcsid, eprefixid);
	exit(EXIT_SUCCESS);
}

void
freeargv(int argc, char **argv)
{
	while (argc--)
		free(argv[argc]);
	free(argv);
}

void
makeargv(const char *string, int *argc, char ***argv)
{
	int curc = 2;
	char *q, *p, *str;
	(*argv) = xmalloc(sizeof(char *) * curc);

	*argc = 1;
	(*argv)[0] = xstrdup(argv0);

	/* shortcut empty strings */
	while (isspace((int)*string))
		string++;
	if (*string == '\0')
		return;

	q = xstrdup(string);
	str = q;

	remove_extra_space(str);
	rmspace(str);

	while (str) {
		if ((p = strchr(str, ' ')) != NULL)
			*(p++) = '\0';

		if (*argc == curc) {
			curc *= 2;
			(*argv) = xrealloc(*argv, sizeof(char *) * curc);
		}
		(*argv)[*argc] = xstrdup(str);
		(*argc)++;
		str = p;
	}
	free(q);
}

static void
strincr_var(const char *name, const char *s, char **value, size_t *value_len)
{
	size_t len;
	char *p, *nv;
	char brace;

	len = strlen(s);
	*value = xrealloc(*value, *value_len + len + 2);
	nv = &(*value)[*value_len];
	if (*value_len)
		*nv++ = ' ';
	memcpy(nv, s, len + 1);

	while ((p = strstr(nv, "-*")) != NULL)
		memset(*value, ' ', p - *value + 2);

	/* This function is mainly used by the startup code for parsing
		make.conf and stacking variables remove.
		variables can be in the form of ${v} or $v
		works:
			FEATURES="${FEATURES} foo"
			FEATURES="$FEATURES foo"
			FEATURES="baz bar -* foo"

		wont work:
			FEATURES="${OTHERVAR} foo"
			FEATURES="-nls nls -nls"
			FEATURES="nls nls nls"
	*/

	len = strlen(name);
	p = nv;
	while ((p = strchr(p, '$')) != NULL) {
		nv = p;
		p++;  /* skip $ */
		brace = *p == '{';
		if (brace)
			p++;
		if (strncmp(p, name, len) == 0) {
			p += len;
			if (brace && *p == '}') {
				p++;
				memset(nv, ' ', p - nv);
			} else if (!brace && (*p == '\0' || isspace((int)*p))) {
				memset(nv, ' ', p - nv);
			}
		}
	}

	remove_extra_space(*value);
	*value_len = strlen(*value);
	/* we should sort here */
}

static env_vars *
get_portage_env_var(env_vars *vars, const char *name)
{
	size_t i;

	for (i = 0; vars[i].name; ++i)
		if (!strcmp(vars[i].name, name))
			return &vars[i];

	return NULL;
}

static void
set_portage_env_var(env_vars *var, const char *value, const char *src)
{
	switch (var->type) {
	case _Q_BOOL:
		*var->value.b = 1;
		free(var->src);
		var->src = xstrdup(src);
		break;
	case _Q_STR:
		free(*var->value.s);
		*var->value.s = xstrdup(value);
		var->value_len = strlen(value);
		free(var->src);
		var->src = xstrdup(src);
		break;
	case _Q_ISTR:
		if (strcmp(var->src, STR_DEFAULT) != 0) {
			size_t l = strlen(var->src) + 2 + strlen(src) + 1;
			char *p = xmalloc(sizeof(char) * l);
			snprintf(p, l, "%s, %s", var->src, src);
			free(var->src);
			var->src = p;
		} else {
			free(*var->value.s);
			*var->value.s = NULL;
			var->value_len = 0;
			free(var->src);
			var->src = xstrdup(src);
		}
		strincr_var(var->name, value, var->value.s, &var->value_len);
		break;
	}
}

/* Helper to read a portage file (e.g. make.conf, package.mask), or
 * recursively if it points to a directory (we don't care about EAPI for
 * dirs, basically PMS 5.2.5 EAPI restriction is ignored) */
enum portage_file_type { ENV_FILE, PMASK_FILE };
static void
read_portage_file(const char *file, enum portage_file_type type, void *data)
{
	FILE *fp;
	struct dirent **dents;
	int dentslen;
	char *s;
	char *p;
	char *buf = NULL;
	size_t buflen = 0;
	size_t line;
	int i;
	env_vars *vars = data;
	set *masks = data;

	if ((dentslen = scandir(file, &dents, NULL, alphasort)) > 0) {
		int di;
		struct dirent *d;
		char npath[_Q_PATH_MAX * 2];

		/* recurse through all files */
		for (di = 0; di < dentslen; di++) {
			d = dents[di];
			if (d->d_name[0] == '.' || d->d_name[0] == '\0' ||
					d->d_name[strlen(d->d_name) - 1] == '~')
				continue;
			snprintf(npath, sizeof(npath), "%s/%s", file, d->d_name);
			read_portage_file(npath, type, data);
		}
		scandir_free(dents, dentslen);
		goto done;
	}

	fp = fopen(file, "r");
	if (fp == NULL)
		goto done;

	line = 0;
	while (getline(&buf, &buflen, fp) != -1) {
		line++;
		rmspace(buf);
		if (*buf == '#' || *buf == '\0')
			continue;

		/* Handle "source" keyword */
		if (type == ENV_FILE) {
			if (strncmp(buf, "source ", 7) == 0) {
				const char *sfile = buf + 7;
				char npath[_Q_PATH_MAX * 2];

				if (sfile[0] != '/') {
					/* handle relative paths */
					size_t file_path_len;

					s = strrchr(file, '/');
					file_path_len = s - file + 1;

					snprintf(npath, sizeof(npath), "%.*s/%s",
							(int)file_path_len, file, sfile);
					sfile = npath;
				}

				read_portage_file(sfile, type, data);
				continue;
			}

			/* look for our desired variables and grab their value */
			for (i = 0; vars[i].name; i++) {
				if (buf[vars[i].name_len] != '=' &&
						buf[vars[i].name_len] != ' ')
					continue;
				if (strncmp(buf, vars[i].name, vars[i].name_len))
					continue;

				/* make sure we handle spaces between the varname, the =,
				 * and the value:
				 * VAR=val   VAR = val   VAR="val"
				 */
				s = buf + vars[i].name_len;
				if ((p = strchr(s, '=')) != NULL)
					s = p + 1;
				while (isspace(*s))
					s++;
				if (*s == '"' || *s == '\'') {
					char *endq;
					char q = *s;

					/* make sure we handle spacing/comments after the quote */
					endq = strchr(s + 1, q);
					if (!endq) {
						/* if the last char is not a quote,
						 * then we span lines */
						size_t abuflen;
						char *abuf;

						abuf = NULL;
						while (getline(&abuf, &abuflen, fp) != -1) {
							buf = xrealloc(buf, buflen + abuflen);
							endq = strchr(abuf, q);
							if (endq)
								*endq = '\0';

							strcat(buf, abuf);
							buflen += abuflen;

							if (endq)
								break;
						}
						free(abuf);

						if (!endq)
							warn("%s:%zu: %s: quote mismatch",
									file, line, vars[i].name);

						s = buf + vars[i].name_len + 2;
					} else {
						*endq = '\0';
						s++;
					}
				} else {
					/* no quotes, so chop the spacing/comments ourselves */
					size_t off = strcspn(s, "# \t\n");
					s[off] = '\0';
				}

				set_portage_env_var(&vars[i], s, file);
			}
		} else if (type == PMASK_FILE) {
			/* trim leading space */
			for (s = buf; isspace((int)*s); s++)
				;
			if (*s == '\0')
				continue;
			if (*s == '-') {
				/* negation/removal, lookup and drop mask if it exists;
				 * note that this only supports exact matches (PMS
				 * 5.2.5) so we don't even have to parse and use
				 * atom-compare here */
				s++;
				if ((p = del_set(s, masks, NULL)) != NULL)
					free(p);
			} else {
				p = xstrdup(file);
				if (add_set_value(s, p, masks) != NULL)
					free(p);
			}
		}
	}

	fclose(fp);
 done:
	free(buf);

	if (getenv("DEBUG"))
		fprintf(stderr, "read profile %s\n", file);
}

/* Helper to recursively read stacked make.defaults in profiles */
static void
read_portage_profile(const char *profile, env_vars vars[], set *masks)
{
	char profile_file[_Q_PATH_MAX * 3];
	char rpath[_Q_PATH_MAX];
	size_t profile_len;
	char *s;
	char *p;
	char *buf = NULL;
	size_t buf_len = 0;
	char *saveptr;

	/* create mutable/appendable copy */
	profile_len = snprintf(profile_file, sizeof(profile_file), "%s/", profile);

	/* check if we have enough space (should always be the case) */
	if (sizeof(profile_file) - profile_len < sizeof("make.defaults"))
		return;

	/* first walk all the parents, PMS 5.2.1 defines that it should
	 * treat parent profiles as defaults, that can be overridden by
	 * *this* profile. */
	strcpy(profile_file + profile_len, "parent");
	if (eat_file(profile_file, &buf, &buf_len)) {
		s = strtok_r(buf, "\n", &saveptr);
		for (; s != NULL; s = strtok_r(NULL, "\n", &saveptr)) {
			/* handle repo: notation (not in PMS, referenced in Wiki only?) */
			if ((p = strchr(s, ':')) != NULL) {
				char *overlay;
				char *repo_name;
				size_t n;

				/* split repo from target */
				*p++ = '\0';

				/* match the repo */
				repo_name = NULL;
				array_for_each(overlays, n, overlay) {
					repo_name = xarrayget(overlay_names, n);
					if (strcmp(repo_name, s) == 0) {
						snprintf(profile_file, sizeof(profile_file),
								"%s/profiles/%s/", overlay, p);
						break;
					}
					repo_name = NULL;
				}
				if (repo_name == NULL) {
					warn("ignoring parent with unknown repo in profile %s: %s",
							profile, s);
					continue;
				}
			} else {
				snprintf(profile_file + profile_len,
						sizeof(profile_file) - profile_len, "%s", s);
			}
			read_portage_profile(
					realpath(profile_file, rpath) == NULL ?
					profile_file : rpath, vars, masks);
			/* restore original path in case we were repointed by profile */
			if (p != NULL)
				snprintf(profile_file, sizeof(profile_file), "%s/", profile);
		}
	}

	if (buf != NULL)
		free(buf);

	/* now consume *this* profile's make.defaults and package.mask */
	strcpy(profile_file + profile_len, "make.defaults");
	read_portage_file(profile_file, ENV_FILE, vars);
	strcpy(profile_file + profile_len, "package.mask");
	read_portage_file(profile_file, PMASK_FILE, masks);
}

static bool nocolor = 0;
env_vars vars_to_read[] = {
#define _Q_EV(t, V, set, lset, d) \
{ \
	.name = #V, \
	.name_len = sizeof(#V) - 1, \
	.type = _Q_##t, \
	set, \
	lset, \
	.default_value = d, \
	.src = NULL, \
},
#define _Q_EVS(t, V, v, d) \
	_Q_EV(t, V, .value.s = &v, .value_len = sizeof(d) - 1, d)
#define _Q_EVB(t, V, v, d) \
	_Q_EV(t, V, .value.b = &v, .value_len = 0, d)

	_Q_EVS(STR,  ROOT,                portroot,            "/")
	_Q_EVS(STR,  ACCEPT_LICENSE,      accept_license,      "")
	_Q_EVS(ISTR, INSTALL_MASK,        install_mask,        "")
	_Q_EVS(ISTR, PKG_INSTALL_MASK,    pkg_install_mask,    "")
	_Q_EVS(STR,  ARCH,                portarch,            "")
	_Q_EVS(ISTR, CONFIG_PROTECT,      config_protect,      "/etc")
	_Q_EVS(ISTR, CONFIG_PROTECT_MASK, config_protect_mask, "")
	_Q_EVB(BOOL, NOCOLOR,             nocolor,             0)
	_Q_EVS(ISTR, FEATURES,            features,            "")
	_Q_EVS(STR,  EPREFIX,             eprefix,             CONFIG_EPREFIX)
	_Q_EVS(STR,  EMERGE_LOG_DIR,      portlogdir,          CONFIG_EPREFIX "var/log")
	_Q_EVS(STR,  PORTDIR,             main_overlay,        CONFIG_EPREFIX "var/db/repos/gentoo")
	_Q_EVS(STR,  PORTAGE_BINHOST,     binhost,             DEFAULT_PORTAGE_BINHOST)
	_Q_EVS(STR,  PORTAGE_TMPDIR,      port_tmpdir,         CONFIG_EPREFIX "var/tmp/portage/")
	_Q_EVS(STR,  PKGDIR,              pkgdir,              CONFIG_EPREFIX "var/cache/binpkgs/")
	_Q_EVS(STR,  Q_VDB,               portvdb,             CONFIG_EPREFIX "var/db/pkg")
	_Q_EVS(STR,  Q_EDB,               portedb,             CONFIG_EPREFIX "var/cache/edb")
	{ NULL, 0, _Q_BOOL, { NULL }, 0, NULL, NULL, }

#undef _Q_EV
#undef _Q_EVS
#undef _Q_EVB
};
set *package_masks = NULL;

/* Handle a single file in the repos.conf format. */
static void
read_one_repos_conf(const char *repos_conf, char **primary)
{
	char rrepo[_Q_PATH_MAX];
	char *main_repo;
	char *repo;
	char *buf = NULL;
	size_t buf_len = 0;
	char *s = NULL;  /* pacify compiler */
	char *p;
	char *q;
	char *r;
	char *e;
	bool do_trim;
	bool is_default;

	if (getenv("DEBUG"))
		fprintf(stderr, "  parse %s\n", repos_conf);

	if (!eat_file(repos_conf, &buf, &buf_len)) {
		if (buf != NULL)
			free(buf);
		return;
	}

	main_repo = NULL;
	repo = NULL;
	for (p = strtok_r(buf, "\n", &s); p != NULL; p = strtok_r(NULL, "\n", &s))
	{
		/* trim trailing whitespace, remove comments, locate = */
		do_trim = true;
		e = NULL;
		for (r = q = s - 2; q >= p; q--) {
			if (do_trim && isspace((int)*q)) {
				*q = '\0';
				r = q - 1;
			} else if (*q == '#') {
				do_trim = true;
				*q = '\0';
				e = NULL;
				r = q - 1;
			} else {
				if (*q == '=')
					e = q;
				do_trim = false;
			}
		}
		/* make q point to the last char */
		q = r;

		if (*p == '[' && *q == ']') {  /* section header */
			repo = p + 1;
			*q = '\0';
			is_default = strcmp(repo, "DEFAULT") == 0;
			continue;
		} else if (*p == '\0') {       /* empty line */
			continue;
		} else if (e == NULL) {        /* missing = */
			continue;
		} else if (repo == NULL) {     /* not in a section */
			continue;
		}

		/* trim off whitespace before = */
		for (r = e - 1; r >= p && isspace((int)*r); r--)
			*r = '\0';
		/* and after the = */
		for (e++; e < q && isspace((int)*e); e++)
			;

		if (is_default && strcmp(p, "main-repo") == 0) {
			main_repo = e;
		} else if (!is_default && strcmp(p, "location") == 0) {
			void *ele;
			size_t n;
			char *overlay;

			/* try not to get confused by symlinks etc. */
			if (realpath(e, rrepo) != NULL)
				e = rrepo;

			array_for_each(overlay_names, n, overlay) {
				if (strcmp(overlay, repo) == 0)
					break;
				overlay = NULL;
			}
			if (overlay != NULL) {
				/* replace overlay */
				ele = array_get_elem(overlay_src, n);
				free(ele);
				array_get_elem(overlay_src, n) = xstrdup(repos_conf);
				ele = array_get_elem(overlays, n);
				free(ele);
				ele = array_get_elem(overlays, n) = xstrdup(e);
			} else {
				ele = xarraypush_str(overlays, e);
				overlay = xarraypush_str(overlay_names, repo);
				xarraypush_str(overlay_src, repos_conf);
			}
			if (main_repo && strcmp(repo, main_repo) == 0)
				*primary = overlay;
		}
	}

	free(buf);
}

/* Handle a possible directory of files. */
static void
read_repos_conf(const char *configroot, const char *repos_conf, char **primary)
{
	char *top_conf, *sub_conf;
	int i, count;
	struct dirent **confs;

	xasprintf(&top_conf, "%s%s", configroot, repos_conf);
	if (getenv("DEBUG"))
		fprintf(stderr, "repos.conf.d scanner %s\n", top_conf);
	count = scandir(top_conf, &confs, NULL, alphasort);
	if (count == -1) {
		if (errno == ENOTDIR)
			read_one_repos_conf(top_conf, primary);
	} else {
		for (i = 0; i < count; ++i) {
			const char *name = confs[i]->d_name;

			if (name[0] == '.' || name[0] == '\0')
				continue;

			/* Exclude backup files (aka files with ~ as postfix). */
			if (name[strlen(name) - 1] == '~')
				continue;

#ifdef DT_UNKNOWN
			if (confs[i]->d_type != DT_UNKNOWN &&
			    confs[i]->d_type != DT_REG &&
			    confs[i]->d_type != DT_LNK)
				continue;
#endif

			xasprintf(&sub_conf, "%s/%s", top_conf, name);

#ifdef DT_UNKNOWN
			if (confs[i]->d_type != DT_REG)
#endif
			{
				struct stat st;
				if (stat(sub_conf, &st) || !S_ISREG(st.st_mode)) {
					free(sub_conf);
					continue;
				}
			}

			read_one_repos_conf(sub_conf, primary);
			free(sub_conf);
		}
		scandir_free(confs, count);
	}
	free(top_conf);
}

static void
initialize_portage_env(void)
{
	size_t i;
	const char *s;
	env_vars *var;
	char pathbuf[_Q_PATH_MAX];
	char rpathbuf[_Q_PATH_MAX];
	const char *configroot = getenv("PORTAGE_CONFIGROOT");
	char *primary_overlay = NULL;

	/* initialize all the properties with their default value */
	for (i = 0; vars_to_read[i].name; ++i) {
		var = &vars_to_read[i];
		if (var->type != _Q_BOOL)
			*var->value.s = xstrdup(var->default_value);
		var->src = xstrdup(STR_DEFAULT);
	}

	package_masks = create_set();

	/* figure out where to find our config files */
	if (!configroot)
		configroot = CONFIG_EPREFIX;

	/* rstrip /-es */
	i = strlen(configroot);
	while (i > 0 && configroot[i - 1] == '/')
		i--;

	/* read overlays first so we can resolve repo references in profile
	 * parent files (non PMS feature?) */
	snprintf(pathbuf, sizeof(pathbuf), "%.*s", (int)i, configroot);
	read_repos_conf(pathbuf, "/usr/share/portage/config/repos.conf",
			&primary_overlay);
	read_repos_conf(pathbuf, "/etc/portage/repos.conf", &primary_overlay);

	/* consider Portage's defaults */
	snprintf(pathbuf, sizeof(pathbuf),
			"%.*s/usr/share/portage/config/make.globals",
			(int)i, configroot);
	read_portage_file(pathbuf, ENV_FILE, vars_to_read);

	/* start with base masks, Portage behaviour PMS 5.2.8 */
	if (primary_overlay != NULL) {
		char *overlay;
		size_t n;
		array_for_each(overlay_names, n, overlay) {
			if (overlay == primary_overlay) {
				snprintf(pathbuf, sizeof(pathbuf), "%s/profiles/package.mask",
						(char *)array_get_elem(overlays, n));
				read_portage_file(pathbuf, PMASK_FILE, package_masks);
				break;
			}
		}
	}

	/* walk all the stacked profiles */
	snprintf(pathbuf, sizeof(pathbuf), "%.*s/etc/make.profile",
			(int)i, configroot);
	read_portage_profile(
			realpath(pathbuf, rpathbuf) == NULL ? pathbuf : rpathbuf,
			vars_to_read, package_masks);
	snprintf(pathbuf, sizeof(pathbuf), "%.*s/etc/portage/make.profile",
			(int)i, configroot);
	read_portage_profile(
			realpath(pathbuf, rpathbuf) == NULL ? pathbuf : rpathbuf,
			vars_to_read, package_masks);

	/* now read all Portage's config files */
	snprintf(pathbuf, sizeof(pathbuf), "%.*s/etc/make.conf",
			(int)i, configroot);
	read_portage_file(pathbuf, ENV_FILE, vars_to_read);
	snprintf(pathbuf, sizeof(pathbuf), "%.*s/etc/portage/make.conf",
			(int)i, configroot);
	read_portage_file(pathbuf, ENV_FILE, vars_to_read);

	/* finally, check the env */
	for (i = 0; vars_to_read[i].name; i++) {
		var = &vars_to_read[i];
		s = getenv(var->name);
		if (s != NULL)
			set_portage_env_var(var, s, var->name);
	}

	/* expand any nested variables e.g. PORTDIR=${EPREFIX}/usr/portage */
	for (i = 0; vars_to_read[i].name; ++i) {
		char *svar;

		var = &vars_to_read[i];
		if (var->type == _Q_BOOL)
			continue;

		while ((svar = strchr(*var->value.s, '$'))) {
			env_vars *evar;
			bool brace;
			const char *sval;
			size_t slen, pre_len, var_len, post_len;
			char byte;

			pre_len = svar - *var->value.s;

			/* First skip the leading "${" */
			s = ++svar;
			brace = (*svar == '{');
			if (brace)
				s = ++svar;

			/* Now skip the variable name itself */
			while (isalnum(*svar) || *svar == '_')
				++svar;

			/* Finally skip the trailing "}" */
			if (brace && *svar != '}') {
				warn("invalid variable setting: %s\n", *var->value.s);
				break;
			}

			var_len = svar - s + 1 + (brace ? 2 : 0);

			byte = *svar;
			*svar = '\0';

			/* Don't try to expand ourselves */
			if (strcmp(var->name, s)) {
				evar = get_portage_env_var(vars_to_read, s);
				if (evar) {
					sval = *evar->value.s;
				} else {
					sval = getenv(s);
					if (!sval)
						sval = "";
				}
			} else {
				sval = "";
			}
			*svar = byte;
			slen = strlen(sval);
			post_len = strlen(svar + !!brace);
			*var->value.s = xrealloc(*var->value.s,
					pre_len + MAX(var_len, slen) + post_len + 1);

			/*
			 * VAR=XxXxX	(slen = 5)
			 * FOO${VAR}BAR
			 * pre_len = 3
			 * var_len = 6
			 * post_len = 3
			 */
			memmove(*var->value.s + pre_len + slen,
				*var->value.s + pre_len + var_len,
				post_len + 1);
			memcpy(*var->value.s + pre_len, sval, slen);
		}
	}

	/* handle PORTDIR and primary_overlay to get a unified
	 * administration in overlays */
	{
		char *overlay;
		var = &vars_to_read[11];  /* PORTDIR */

		if (strcmp(var->src, STR_DEFAULT) != 0 || array_cnt(overlays) == 0) {
			char roverlay[_Q_PATH_MAX];
			/* get cannonical path, we do so for repos.conf too */
			if (realpath(main_overlay, roverlay) == NULL)
				snprintf(roverlay, sizeof(roverlay), "%s", main_overlay);
			array_for_each(overlays, i, overlay) {
				if (strcmp(overlay, roverlay) == 0)
					break;
				overlay = NULL;
			}
			if (overlay == NULL) {  /* add PORTDIR to overlays */
				free(main_overlay);
				main_overlay = xstrdup(roverlay);
				xarraypush_ptr(overlays, main_overlay);
				xarraypush_str(overlay_names, "<PORTDIR>");
				xarraypush_str(overlay_src, var->src);
			} else {
				/* ignore make.conf and/or env setting origin if defined by
				 * repos.conf since the former are deprecated */
				free(main_overlay);
			}
			main_overlay = NULL;  /* now added to overlays */
		} else {
			free(main_overlay);
		}

		/* set main_overlay to the one pointed to by repos.conf, if any */
		i = 0;
		if (primary_overlay != NULL) {
			array_for_each(overlay_names, i, overlay) {
				if (overlay == primary_overlay)
					break;
				overlay = NULL;
			}
			/* if no explicit overlay was flagged as main, take the
			 * first one */
			if (overlay == NULL)
				i = 0;
		}
		main_overlay = array_get_elem(overlays, i);
		/* set source for PORTDIR var */
		free(var->src);
		var->src = xstrdup((char *)array_get_elem(overlay_src, i));
	}

	/* Make sure ROOT always ends in a slash */
	var = &vars_to_read[0];
	if (var->value_len == 0 || (*var->value.s)[var->value_len - 1] != '/') {
		portroot = xrealloc(portroot, var->value_len + 2);
		portroot[var->value_len] = '/';
		portroot[var->value_len + 1] = '\0';
	}

	if (getenv("DEBUG")) {
		for (i = 0; vars_to_read[i].name; ++i) {
			var = &vars_to_read[i];
			fprintf(stderr, "%s = ", var->name);
			switch (var->type) {
			case _Q_BOOL: fprintf(stderr, "%i\n", *var->value.b); break;
			case _Q_STR:
			case _Q_ISTR: fprintf(stderr, "%s\n", *var->value.s); break;
			}
		}
	}

	if (getenv("PORTAGE_QUIET") != NULL)
		setup_quiet();

	if (nocolor)
		no_colors();
	else
		color_remap();
}

int main(int argc, char **argv)
{
	struct stat st;
	struct winsize winsz;

	warnout = stderr;
	IF_DEBUG(init_coredumps());
	argv0 = argv[0];

	setlocale(LC_ALL, "");
	bindtextdomain(argv0, CONFIG_EPREFIX "usr/share/locale");
	textdomain(argv0);

	twidth = 0;
	if (fstat(fileno(stdout), &st) != -1) {
		if (!isatty(fileno(stdout))) {
			no_colors();
		} else {
			if ((getenv("TERM") == NULL) ||
					(strcmp(getenv("TERM"), "dumb") == 0))
				no_colors();
			if (ioctl(0, TIOCGWINSZ, &winsz) == 0 && winsz.ws_col > 0)
				twidth = (int)winsz.ws_col;
		}
	}

	initialize_portage_env();
	optind = 0;
	return q_main(argc, argv);
}
