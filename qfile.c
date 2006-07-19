/*
 * Copyright 2005-2006 Gentoo Foundation
 * Distributed under the terms of the GNU General Public License v2
 * $Header: /var/cvsroot/gentoo-projects/portage-utils/qfile.c,v 1.34 2006/07/19 16:20:41 solar Exp $
 *
 * Copyright 2005-2006 Ned Ludd        - <solar@gentoo.org>
 * Copyright 2005-2006 Mike Frysinger  - <vapier@gentoo.org>
 */

#ifdef APPLET_qfile

#define QFILE_FLAGS "eo" COMMON_FLAGS
static struct option const qfile_long_opts[] = {
	{"exact",       no_argument, NULL, 'e'},
	{"orphans",     no_argument, NULL, 'o'},
	COMMON_LONG_OPTS
};
static const char *qfile_opts_help[] = {
	"Exact match",
	"List orphan files",
	COMMON_OPTS_HELP
};
static char qfile_rcsid[] = "$Id: qfile.c,v 1.34 2006/07/19 16:20:41 solar Exp $";
#define qfile_usage(ret) usage(ret, QFILE_FLAGS, qfile_long_opts, qfile_opts_help, lookup_applet_idx("qfile"))

void qfile(char *path, int argc, char **base_names, char **dir_names, char **real_dir_names, short * non_orphans);
void qfile(char *path, int argc, char **base_names, char **dir_names, char **real_dir_names, short * non_orphans)
{
	FILE *fp;
	DIR *dir;
	struct dirent *dentry;
	char *entry_basename;
	char *entry_dirname;
	char *p;
	char buf[1024];
	char pkg[126];
	depend_atom *atom;
	int i, path_ok;

	if (chdir(path) != 0 || (dir = opendir(".")) == NULL)
		return;

	while ((dentry = readdir(dir))) {
		if (dentry->d_name[0] == '.')
			continue;
		xasprintf(&p, "%s/%s/CONTENTS", path, dentry->d_name);
		if ((fp = fopen(p, "r")) == NULL) {
			free(p);
			continue;
		}
		free(p);

		snprintf(pkg, sizeof(pkg), "%s/%s", basename(path), dentry->d_name);
		while ((fgets(buf, sizeof(buf), fp)) != NULL) {
			contents_entry *e;
			e = contents_parse_line(buf);
			if (!e)
				continue;

			// much faster than using basename(), since no need to strdup
			if ((entry_basename = strrchr(e->name, '/')) == NULL)
				continue;
			entry_basename++;

			for(i = 0; i < argc; i++) {
				if (base_names[i] == NULL)
					continue;
				if (non_orphans != NULL && non_orphans[i])
					continue;
				path_ok = (dir_names[i] == NULL && real_dir_names[i] == NULL);

				if (strcmp(entry_basename, base_names[i]) != 0)
					continue;

				if (!path_ok) {
					// check the full filepath...
					entry_dirname = xstrdup(e->name);
					if ((p = strrchr(entry_dirname, '/')) == NULL) {
						free(entry_dirname);
						continue;
					}
					if (p == entry_dirname)
						// (e->name == "/foo") ==> dirname == "/"
						*(p + 1) = '\0';
					else
						*p = '\0';
					if (dir_names[i] != NULL && 
							strcmp(entry_dirname, dir_names[i]) == 0)
						// dir_name == dirname(CONTENTS)
						path_ok = 1;
					else if (real_dir_names[i] != NULL && 
							strcmp(entry_dirname, real_dir_names[i]) == 0)
						// real_dir_name == dirname(CONTENTS)
						path_ok = 1;
					else {
						char rpath[_Q_PATH_MAX];
						errno = 0;
						realpath(entry_dirname, rpath);
						if (errno != 0) {
							if (verbose) {
								warn("Could not read real path of \"%s\": %s", p, strerror(errno));
								warn("We'll never know whether it was a result for your query...");
							}
						} else if (dir_names[i] != NULL && 
								strcmp(rpath, dir_names[i]) == 0)
							// dir_name == realpath(dirname(CONTENTS))
							path_ok = 1;
						else if (real_dir_names[i] != NULL && 
								strcmp(rpath, real_dir_names[i]) == 0)
							// real_dir_name == realpath(dirname(CONTENTS))
							path_ok = 1;
					}
					free(entry_dirname);
				}
				if (!path_ok)
					continue;

				if (non_orphans == NULL) {
					if ((atom = atom_explode(pkg)) == NULL) {
						warn("invalid atom %s", pkg);
						continue;
					}

					printf("%s%s/%s%s%s", BOLD, atom->CATEGORY, BLUE,
						(exact ? dentry->d_name : atom->PN), NORM);
					if (quiet)
						puts("");
					else
						printf(" (%s)\n", e->name);
	
					atom_implode(atom);
				} else {
					non_orphans[i] = 1;
				}
				found++;
			}
		}
		fclose(fp);
	}
	closedir(dir);

	return;
}

int qfile_main(int argc, char **argv)
{
	DIR *dir;
	struct dirent *dentry;
	int i;
	char *p;
	char ** basenames;
	char ** dirnames;
	char ** realdirnames;
	char * pwd;
	short * non_orphans = NULL;
	short search_orphans = 0;

	DBG("argc=%d argv[0]=%s argv[1]=%s",
	    argc, argv[0], argc > 1 ? argv[1] : "NULL?");

	while ((i = GETOPT_LONG(QFILE, qfile, "")) != -1) {
		switch (i) {
			COMMON_GETOPTS_CASES(qfile)
			case 'e': exact = 1; break;
			case 'o': search_orphans = 1; break;
		}
	}
	if (!exact && verbose) exact++;
	if (argc == optind)
		qfile_usage(EXIT_FAILURE);

	if (chdir(portroot))
		errp("could not chdir(%s) for ROOT", portroot);

	if (chdir(portvdb) != 0 || (dir = opendir(".")) == NULL)
		return EXIT_FAILURE;

	if (search_orphans)
		non_orphans = xmalloc((argc-optind) * sizeof(short));

	// For each argument, we store its basename, its dirname,
	// and the realpath of its dirname.
	basenames = xmalloc((argc-optind) * sizeof(char*));
	dirnames = xmalloc((argc-optind) * sizeof(char*));
	realdirnames = xmalloc((argc-optind) * sizeof(char*));
	if ((pwd = getenv("PWD")) != NULL) {
		int pwdlen = strlen(pwd);
		if ((pwdlen > 0) && (pwd[pwdlen-1] == '/'))
			pwd[pwdlen-1] = '\0';
	}
	for (i = 0; i < (argc-optind); ++i) {
		char tmppath[_Q_PATH_MAX];
		char abspath[_Q_PATH_MAX];

		basenames[i] = NULL;
		dirnames[i] = NULL;
		realdirnames[i] = NULL;

		// Record basename, but if it is "." or ".."
		strncpy(tmppath, basename(argv[i+optind]), _Q_PATH_MAX);
		if ((strlen(tmppath) > 2) || strncmp(tmppath, "..", strlen(tmppath))) {
			basenames[i] = xstrdup(tmppath);
			// If there is no "/" in the argument, then it's over.
			// (we are searching a simple file name)
			if (strchr(argv[i+optind], '/') == NULL)
				continue;
		}

		// Make sure we have an absolute path available
		if (argv[i+optind][0] == '/')
			strncpy(abspath, argv[i+optind], _Q_PATH_MAX);
		else if (pwd != NULL)
			snprintf(abspath, _Q_PATH_MAX, "%s/%s", pwd, argv[i+optind]);
		else {
			warn("$PWD not found in environment.");
			warn("Skipping query item \"%s\".", argv[i+optind]);
			continue;
		}

		if (basenames[i] != NULL) {
			// Get both the dirname and its realpath
			dirnames[i] = xstrdup(dirname(abspath));
			errno = 0;
			realpath(dirnames[i], tmppath);
			if (errno != 0) {
				if (verbose) {
					warn("Could not read real path of \"%s\": %s", dirnames[i], strerror(errno));
					warn("Results for query item \"%s\" may not be accurate.", argv[i+optind]);
				}
			} else if (strcmp(dirnames[i], tmppath))
				realdirnames[i] = xstrdup(tmppath);
		} else {
			// No basename means we are looking for something like "/foo/bar/.."
			// Dirname is meaningless here, we can only get realpath of the full 
			// path and then split it.
			errno = 0;
			realpath(tmppath, abspath);
			if (errno != 0) {
				warn("Could not read real path of \"%s\": %s", tmppath, strerror(errno));
				warn("Skipping query item \"%s\".", argv[i+optind]);
				continue;
			}
			basenames[i] = xstrdup(basename(tmppath));
			realdirnames[i] = xstrdup(dirname(tmppath));
		}
	}

	/* open /var/db/pkg */
	while ((dentry = q_vdb_get_next_dir(dir))) {
		xasprintf(&p, "%s%s/%s", portroot, portvdb, dentry->d_name);
		qfile(p, (argc-optind), basenames, dirnames, realdirnames, non_orphans);
		free(p);
	}

	if (non_orphans != NULL) {
		// display orphan files
		for (i = 0; i < (argc-optind); i++) {
			if (non_orphans[i]) 
				continue;
			if (basenames[i] != NULL) {
				found = 0; // ~inverse return code (as soon as an orphan is found, return non-zero)
				if (!quiet)
					printf("%s\n", argv[i+optind]);
			}
		}
		free(non_orphans);
	}

	for (i = 0; i < (argc-optind); ++i) {
		if (basenames[i] != NULL) free(basenames[i]);
		if (dirnames[i] != NULL) free(dirnames[i]);
		if (realdirnames[i] != NULL) free(realdirnames[i]);
	}
	free(basenames); free(dirnames); free(realdirnames);

	return (found ? EXIT_SUCCESS : EXIT_FAILURE);
}

#else
DEFINE_APPLET_STUB(qfile)
#endif
