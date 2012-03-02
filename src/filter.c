/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "fileops.h"
#include "hash.h"
#include "filter.h"
#include "repository.h"
#include "git2/config.h"

/* Fresh from Core Git. I wonder what we could use this for... */
void git_text_gather_stats(git_text_stats *stats, const git_buf *text)
{
	size_t i;

	memset(stats, 0, sizeof(*stats));

	for (i = 0; i < text->size; i++) {
		unsigned char c = text->ptr[i];

		if (c == '\r') {
			stats->cr++;

			if (i + 1 < text->size && text->ptr[i + 1] == '\n')
				stats->crlf++;

			continue;
		}

		if (c == '\n') {
			stats->lf++;
			continue;
		}

		if (c == 127)
			/* DEL */
			stats->nonprintable++;

		else if (c < 32) {
			switch (c) {
				/* BS, HT, ESC and FF */
			case '\b': case '\t': case '\033': case '\014':
				stats->printable++;
				break;
			case 0:
				stats->nul++;
				/* fall through */
			default:
				stats->nonprintable++;
			}
		}
		else
			stats->printable++;
	}

	/* If file ends with EOF then don't count this EOF as non-printable. */
	if (text->size >= 1 && text->ptr[text->size - 1] == '\032')
		stats->nonprintable--;
}

/*
 * Fresh from Core Git
 */
int git_text_is_binary(git_text_stats *stats)
{
	if (stats->nul)
		return 1;

	if ((stats->printable >> 7) < stats->nonprintable)
		return 1;
	/*
	 * Other heuristics? Average line length might be relevant,
	 * as might LF vs CR vs CRLF counts..
	 *
	 * NOTE! It might be normal to have a low ratio of CRLF to LF
	 * (somebody starts with a LF-only file and edits it with an editor
	 * that adds CRLF only to lines that are added..). But do  we
	 * want to support CR-only? Probably not.
	 */
	return 0;
}

static int load_repository_settings(git_repository *repo)
{
	static git_cvar_map map_eol[] = {
		{GIT_CVAR_FALSE, NULL, GIT_EOL_UNSET},
		{GIT_CVAR_STRING, "lf", GIT_EOL_LF},
		{GIT_CVAR_STRING, "crlf", GIT_EOL_CRLF},
		{GIT_CVAR_STRING, "native", GIT_EOL_NATIVE}
	};

	static git_cvar_map map_crlf[] = {
		{GIT_CVAR_FALSE, NULL, GIT_AUTO_CRLF_FALSE},
		{GIT_CVAR_TRUE, NULL, GIT_AUTO_CRLF_TRUE},
		{GIT_CVAR_STRING, "input", GIT_AUTO_CRLF_INPUT}
	};

	git_config *config;
	int error;

	if (repo->filter_options.loaded)
		return GIT_SUCCESS;

	repo->filter_options.eol = GIT_EOL_DEFAULT;
	repo->filter_options.auto_crlf = GIT_AUTO_CRLF_DEFAULT;

	error = git_repository_config__weakptr(&config, repo);
	if (error < GIT_SUCCESS)
		return error;

	error = git_config_get_mapped(
		config, "core.eol", map_eol, ARRAY_SIZE(map_eol), &repo->filter_options.eol);

	if (error < GIT_SUCCESS && error != GIT_ENOTFOUND)
		return error;

	error = git_config_get_mapped(
		config, "core.auto_crlf", map_crlf, ARRAY_SIZE(map_crlf), &repo->filter_options.auto_crlf);

	if (error < GIT_SUCCESS && error != GIT_ENOTFOUND)
		return error;

	repo->filter_options.loaded = 1;
	return 0;
}

int git_filters_load(git_vector *filters, git_repository *repo, const char *path, int mode)
{
	int error;

	/* Make sure that the relevant settings from `gitconfig` have been
	 * cached on the repository struct to speed things up */
	error = load_repository_settings(repo);
	if (error < GIT_SUCCESS)
		return error;

	if (mode == GIT_FILTER_TO_ODB) {
		/* Load the CRLF cleanup filter when writing to the ODB */
		error = git_filter_add__crlf_to_odb(filters, repo, path);
		if (error < GIT_SUCCESS)
			return error;
	} else {
		return git__throw(GIT_ENOTIMPLEMENTED,
			"Worktree filters are not implemented yet");
	}

	return (int)filters->length;
}

void git_filters_free(git_vector *filters)
{
	size_t i;
	git_filter *filter;

	git_vector_foreach(filters, i, filter) {
		if (filter->do_free != NULL)
			filter->do_free(filter);
		else
			free(filter);
	}

	git_vector_free(filters);
}

int git_filters_apply(git_buf *dest, git_buf *source, git_vector *filters)
{
	unsigned int src, dst, i;
	git_buf *dbuffer[2];

	dbuffer[0] = source;
	dbuffer[1] = dest;

	src = 0;

	if (source->size == 0) {
		git_buf_clear(dest);
		return GIT_SUCCESS;
	}

	/* Pre-grow the destination buffer to more or less the size
	 * we expect it to have */
	if (git_buf_grow(dest, source->size) < 0)
		return GIT_ENOMEM;

	for (i = 0; i < filters->length; ++i) {
		git_filter *filter = git_vector_get(filters, i);
		dst = (src + 1) % 2;

		git_buf_clear(dbuffer[dst]);

		/* Apply the filter, from dbuffer[src] to dbuffer[dst];
		 * if the filtering is canceled by the user mid-filter,
		 * we skip to the next filter without changing the source
		 * of the double buffering (so that the text goes through
		 * cleanly).
		 */
		if (filter->apply(filter, dbuffer[dst], dbuffer[src]) == 0) {
			src = (src + 1) % 2;
		}

		if (git_buf_oom(dbuffer[dst]))
			return GIT_ENOMEM;
	}

	/* Ensure that the output ends up in dbuffer[1] (i.e. the dest) */
	if (dst != 1) {
		git_buf_swap(dest, source);
	}

	return GIT_SUCCESS;
}
