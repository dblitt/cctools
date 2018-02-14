/*
 Copyright (C) 2018- The University of Notre Dame
 This software is distributed under the GNU General Public License.
 See the file COPYING for details.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "batch_task.h"
#include "batch_wrapper.h"
#include "debug.h"
#include "path.h"
#include "stringtools.h"
#include "xxmalloc.h"

#include "dag.h"
#include "dag_file.h"
#include "makeflow_gc.h"
#include "makeflow_log.h"
#include "makeflow_hook.h"

#define CONTAINER_DOCKER_SH "./docker.wrapper.sh_"

char *docker_image = NULL;

char *docker_tar = NULL;

static int create( struct jx *hook_args )
{
    if(jx_lookup_string(hook_args, "docker_container_image"))
        docker_image = xxstrdup(jx_lookup_string(hook_args, "docker_container_image"));	

    if(jx_lookup_string(hook_args, "docker_container_tar"))
        docker_tar = xxstrdup(jx_lookup_string(hook_args, "docker_container_tar"));	

	return MAKEFLOW_HOOK_SUCCESS;
}

static int dag_check(struct dag *d){
	char *cwd = path_getcwd();
	if(!strncmp(cwd, "/afs", 4)) {
		fprintf(stderr,"error: The working directory is '%s'\n", cwd);
		fprintf(stderr,"This won't work because Docker cannot mount an AFS directory.\n");
		fprintf(stderr,"Instead, run your workflow from a local disk like /tmp.");
		fprintf(stderr,"Or, use the Work Queue batch system with -T wq.\n");
		free(cwd);
		return MAKEFLOW_HOOK_FAILURE;
	}
	free(cwd);
	return MAKEFLOW_HOOK_SUCCESS;
}

static int node_submit(struct dag_node *n, struct batch_task *t){
	struct batch_wrapper *wrapper = batch_wrapper_create();
	batch_wrapper_prefix(wrapper, CONTAINER_DOCKER_SH);

	/* Save the directory we were originally working in. */
	batch_wrapper_pre(wrapper, "export CUR_WORK_DIR=$(pwd)");
	batch_wrapper_pre(wrapper, "export DEFAULT_DIR=/root/worker");

	if (docker_tar == NULL) {
		char *pull = string_format("flock /tmp/lockfile /usr/bin/docker pull %s", docker_image);
		batch_wrapper_pre(wrapper, pull);
		free(pull);
	} else {
		char *load = string_format("flock /tmp/lockfile /usr/bin/docker load < %s", docker_tar);
		batch_wrapper_pre(wrapper, load);
		free(load);
		makeflow_hook_add_input_file(n->d, t, docker_tar, NULL, DAG_FILE_TYPE_GLOBAL);
	}
	makeflow_hook_add_input_file(n->d, t, docker_image, NULL, DAG_FILE_TYPE_GLOBAL);

	char *cmd = string_format("docker run --rm -m 1g -v $CUR_WORK_DIR:$DEFAULT_DIR -w $DEFAULT_DIR %s %s", docker_image, t->command);
	batch_wrapper_cmd(wrapper, cmd);
	free(cmd);

	cmd = batch_wrapper_write(wrapper, t);
	if(cmd){
		batch_task_set_command(t, cmd);
		struct dag_file *df = makeflow_hook_add_input_file(n->d, t, cmd, cmd, DAG_FILE_TYPE_TEMP);
		debug(D_MAKEFLOW_HOOK, "Wrapper written to %s", df->filename);
		makeflow_log_file_state_change(n->d, df, DAG_FILE_STATE_EXISTS);
	} else {
		debug(D_MAKEFLOW_HOOK, "Failed to create wrapper: errno %d, %s", errno, strerror(errno));
		return MAKEFLOW_HOOK_FAILURE;
	}
	free(cmd);

	return MAKEFLOW_HOOK_SUCCESS;
}
	
struct makeflow_hook makeflow_hook_docker = {
	.module_name = "Docker",
	.create = create,

	.dag_check = dag_check,

	.node_submit = node_submit,
};


