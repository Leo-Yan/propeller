/*
 * Copyright (C) 2020-2021 Seagate
 * Copyright (C) 2020-2021 Linaro Ltd
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <blkid/blkid.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>

#include "ilm.h"

#include "drive.h"
#include "list.h"
#include "log.h"

#define SYSFS_ROOT		"/sys"
#define BUS_SCSI_DEVS		"/bus/scsi/devices"

struct ilm_hw_drive_path {
	char *blk_path;
	char *sg_path;
};

struct ilm_hw_drive {
	uuid_t id;
	uint32_t path_num;
	struct ilm_hw_drive_path path[ILM_DRIVE_MAX_NUM];
};

struct ilm_hw_drive_node {
	struct list_head list;
	struct ilm_hw_drive drive;
};

static struct list_head drive_list;
static char blk_str[PATH_MAX];

int ilm_read_blk_uuid(char *dev, uuid_t *uuid)
{
#ifdef IDM_PTHREAD_EMULATION
	uuid_t id;

	uuid_generate(id);
	memcpy(uuid, &id, sizeof(uuid_t));
	return 0;
#else
	blkid_probe probe;
	const char *uuid_str;
	size_t uuid_str_size;
	uuid_t id;
	int ret;

	probe = blkid_new_probe_from_filename(dev);
	if (!probe) {
		ilm_log_err("fail to create blkid probe for %s", dev);
		return -1;
	}

	blkid_do_probe(probe);

	ret = blkid_probe_lookup_value(probe, "UUID",
				       &uuid_str, &uuid_str_size);
	if (ret) {
		ilm_log_warn("fail to lookup blkid value %s", dev);
		memset(uuid, 0x0, sizeof(uuid_t));
	} else {
		ilm_log_dbg("blkid uuid_str %s", uuid_str);
		uuid_parse(uuid_str, id);
		memcpy(uuid, &id, sizeof(uuid_t));
	}

	blkid_free_probe(probe);
	return ret;
#endif
}

int ilm_read_parttable_id(char *dev, uuid_t *uuid)
{
#ifdef IDM_PTHREAD_EMULATION
	*id = malloc(sizeof(uuid_t));

	uuid_generate(id);
	memcpy(uuid, &id, sizeof(uuid_t));
	return 0;
#else
	blkid_probe probe;
	blkid_partlist ls;
	blkid_parttable root_tab;
	const char *uuid_str;
	uuid_t id;

	probe = blkid_new_probe_from_filename(dev);
	if (!probe) {
		ilm_log_err("fail to create blkid probe for %s", dev);
		return -1;
	}

	/* Binary interface */
	ls = blkid_probe_get_partitions(probe);
	if (!ls) {
		ilm_log_err("fail to read partitions for %s", dev);
		return -1;
	}

	root_tab = blkid_partlist_get_table(ls);
	if (!root_tab) {
		ilm_log_err("doesn't contains any partition table %s", dev);
		return -1;
	}

	uuid_str = blkid_parttable_get_id(root_tab);
	if (!uuid_str) {
		ilm_log_err("fail to read partition table id %s", dev);
		return -1;
	}

	ilm_log_dbg("blkid parttable uuid_str %s", uuid_str);
	uuid_parse(uuid_str, id);
	memcpy(uuid, &id, sizeof(uuid_t));

	blkid_free_probe(probe);
	return 0;
#endif
}

#ifndef IDM_PTHREAD_EMULATION
static int ilm_scsi_dir_select(const struct dirent *s)
{
	/* Following no longer needed but leave for early lk 2.6 series */
	if (strstr(s->d_name, "mt"))
		return 0;

	/* st auxiliary device names */
	if (strstr(s->d_name, "ot"))
		return 0;

	/* osst auxiliary device names */
	if (strstr(s->d_name, "gen"))
		return 0;

	/* SCSI host */
	if (!strncmp(s->d_name, "host", 4))
		return 0;

	/* SCSI target */
	if (!strncmp(s->d_name, "target", 6))
		return 0;

	/* Only select directory with x:x:x:x */
	if (strchr(s->d_name, ':'))
		return 1;

	return 0;
}

static int ilm_scsi_change_sg_folder(const char *dir_name)
{
        const char *old_name = "generic";
        char b[PATH_MAX];
        struct stat a_stat;
	int ret;

        ret = snprintf(b, sizeof(b), "%s/%s", dir_name, old_name);
	if (ret < 0) {
		ilm_log_err("%s: spring is out of range", __func__);
		return -1;
	}

        if ((stat(b, &a_stat) >= 0) && S_ISDIR(a_stat.st_mode)) {
                if (chdir(b) < 0)
                        return -1;

                return 0;
        }

        return -1;
}

static int ilm_scsi_parse_sg_node(unsigned int maj, unsigned int min,
				  char *dev)
{
	struct dirent *dep;
	DIR *dirp;
	char path[PATH_MAX];
	struct stat stats;

	dirp = opendir("/dev");
	if (dirp == NULL)
		return -1;

	while (1) {
		dep = readdir(dirp);
		if (dep == NULL)
			break;

		snprintf(path, sizeof(path), "%s/%s", "/dev", dep->d_name);

		/* This will bypass all symlinks in /dev */
		if (lstat(path, &stats))
			continue;

		/* Skip non-block/char files. */
		if ( (!S_ISBLK(stats.st_mode)) && (!S_ISCHR(stats.st_mode)) )
			continue;

		if (major(stats.st_rdev) == maj && minor(stats.st_rdev) == min) {
			strcpy(dev, path);
			return 0;
		}
	}

	return -1;
}

static int ilm_scsi_block_node_select(const struct dirent *s)
{
	size_t len;

	if (DT_LNK != s->d_type && DT_DIR != s->d_type)
		return 0;

	if (DT_DIR == s->d_type) {
		len = strlen(s->d_name);

		if ((len == 1) && ('.' == s->d_name[0]))
			return 0;   /* this directory: '.' */

		if ((len == 2) &&
		    ('.' == s->d_name[0]) && ('.' == s->d_name[1]))
			return 0;   /* parent: '..' */
	}

	strncpy(blk_str, s->d_name, PATH_MAX);
	return 1;
}

static int ilm_scsi_find_block_node(const char *dir_name)
{
        int num, i;
        struct dirent **namelist;

        num = scandir(dir_name, &namelist, ilm_scsi_block_node_select, NULL);
        if (num < 0)
                return -1;

        for (i = 0; i < num; ++i)
                free(namelist[i]);
        free(namelist);
        return num;
}

static int ilm_scsi_get_value(const char *dir_name, const char *base_name,
			      char *value, int max_value_len)
{
        int len;
        FILE * f;
        char b[PATH_MAX];
	int ret;

        ret = snprintf(b, sizeof(b), "%s/%s", dir_name, base_name);
	if (ret < 0)
		return -1;

        if (NULL == (f = fopen(b, "r"))) {
                return -1;
        }

        if (NULL == fgets(value, max_value_len, f)) {
                /* assume empty */
                value[0] = '\0';
                fclose(f);
                return 0;
        }

        len = strlen(value);
        if ((len > 0) && (value[len - 1] == '\n'))
                value[len - 1] = '\0';

        fclose(f);
        return 0;
}

static char *ilm_find_sg(char *blk_dev)
{
	struct dirent **namelist;
	char devs_path[PATH_MAX];
	char dev_path[PATH_MAX];
	char blk_path[PATH_MAX];
	int i, num;
	int ret;
	char value[64];
	unsigned int maj, min;
        struct stat a_stat;
	char *tmp = NULL;

	snprintf(devs_path, sizeof(devs_path), "%s%s",
		 SYSFS_ROOT, BUS_SCSI_DEVS);

	num = scandir(devs_path, &namelist, ilm_scsi_dir_select, NULL);
	if (num < 0) {  /* scsi mid level may not be loaded */
		ilm_log_err("Attached devices: none");
		return NULL;
	}

	for (i = 0; i < num; ++i) {
		ret = snprintf(dev_path, sizeof(dev_path), "%s/%s",
			       devs_path, namelist[i]->d_name);
		if (ret < 0) {
			ilm_log_err("string is out of memory\n");
			goto out;
		}

		ret = snprintf(blk_path, sizeof(blk_path), "%s/%s/%s",
			       dev_path, "block", blk_dev);
		if (ret < 0) {
			ilm_log_err("string is out of memory");
			goto out;
		}

		/* The folder doesn't exist */
		if ((stat(blk_path, &a_stat) < 0))
			continue;

		ret = ilm_scsi_change_sg_folder(dev_path);
		if (ret < 0) {
			ilm_log_err("fail to change sg folder");
			goto out;
		}

		if (NULL == getcwd(blk_path, sizeof(blk_path))) {
			ilm_log_err("generic_dev error");
			goto out;
		}

		ret = ilm_scsi_get_value(blk_path, "dev", value, sizeof(value));
		if (ret < 0) {
			ilm_log_err("fail to get device value");
			goto out;
		}

		sscanf(value, "%u:%u", &maj, &min);

		ret = ilm_scsi_parse_sg_node(maj, min, value);
		if (ret < 0) {
			ilm_log_err("fail to find blk node %d:%d\n", maj, min);
			goto out;
		}

		tmp = strdup(value);
	}

out:
        for (i = 0; i < num; i++)
                free(namelist[i]);
	free(namelist);
        return tmp;
}
#endif

char *ilm_scsi_convert_blk_name(char *blk_dev)
{
	FILE *fp;
	char tmp[128];
	char cmd[128];
	char buf[128];
	char *base_name;
	char *blk_name;
	unsigned int num;
	int i;

	ilm_log_dbg("blk_dev %s", blk_dev);

	strncpy(tmp, blk_dev, sizeof(tmp));

	if (strstr(tmp, "/dev/mapper")) {

		snprintf(cmd, sizeof(cmd),
			 "dmsetup deps -o devname %s", tmp);

		if ((fp = popen(cmd, "r")) == NULL) {
			ilm_log_err("Fail to execute command %s", cmd);
			return NULL;
		}

		if (fgets(buf, sizeof(buf), fp) == NULL) {
			ilm_log_err("Fail to read command buffer %s", cmd);
			return NULL;
		}

		pclose(fp);

		sscanf(buf, "%u dependencies  : (%[a-z])", &num, tmp);
		if (num != 1) {
			ilm_log_dbg("Fail to parse device mapper %s", tmp);
			return NULL;
		}

		ilm_log_dbg("num %d dev %s", num, tmp);
	}

	i = strlen(tmp);
	if (!i)
		return NULL;

	/* Iterate all digital */
	while ((i > 0) && isdigit(tmp[i-1]))
		i--;

	tmp[i] = '\0';

	base_name = basename(tmp);
	blk_name = malloc(strlen(base_name) + 1);
	strncpy(blk_name, base_name, strlen(base_name) + 1);

	ilm_log_dbg("blk_name %s", blk_name);
	return blk_name;
}

char *ilm_convert_sg(char *blk_dev)
{
#ifdef IDM_PTHREAD_EMULATION
	char *sg;

	sg = strdup(blk_dev);
	return sg;
#else
	char *tmp = strdup(blk_dev);
	char *sg;
	char cmd[128];
	char buf[128];
	unsigned int num;
	int i;
	FILE *fp;

	if (strstr(tmp, "/dev/mapper")) {
		snprintf(cmd, sizeof(cmd),
			 "dmsetup deps -o devname %s", tmp);

		if ((fp = popen(cmd, "r")) == NULL)
			goto failed;

		if (fgets(buf, sizeof(buf), fp) == NULL)
			goto failed;

		sscanf(buf, "%u dependencies  : (%[a-z])", &num, tmp);
		pclose(fp);
		ilm_log_dbg("num %d dev %s", num, tmp);
	}

	i = strlen(tmp);
	if (!i || !tmp)
		goto failed;

	/* Iterate all digital */
	while ((i > 0) && isdigit(tmp[i-1]))
		i--;

	tmp[i] = '\0';

	sg = ilm_find_sg(basename(tmp));

	free(tmp);
	return sg;

failed:
	if (tmp)
		free(tmp);
	return NULL;
#endif
}

char *ilm_scsi_get_first_sg(char *dev)
{
	struct ilm_hw_drive_node *pos, *found = NULL;
	char *tmp;
	int i;

	list_for_each_entry(pos, &drive_list, list) {
		for (i = 0; i < pos->drive.path_num; i++) {
			if (!strcmp(dev, basename(pos->drive.path[i].blk_path))) {
				found = pos;
				break;
			}
		}
	}

	if (!found)
		return NULL;

	tmp = strdup(found->drive.path[0].sg_path);
	return tmp;
}

int ilm_scsi_get_part_table_uuid(char *dev, uuid_t *id)
{
	struct ilm_hw_drive_node *pos, *found = NULL;
	int i;

	list_for_each_entry(pos, &drive_list, list) {
		for (i = 0; i < pos->drive.path_num; i++) {
			if (!strcmp(basename(dev),
				    basename(pos->drive.path[i].blk_path))) {
				found = pos;
				break;
			}

			if (!strcmp(basename(dev),
				    basename(pos->drive.path[i].sg_path))) {
				found = pos;
				break;
			}
		}
	}

	if (!found)
		return -1;

	memcpy(id, &found->drive.id, sizeof(uuid_t));
	return 0;
}

static void ilm_scsi_dump_nodes(void)
{
	struct ilm_hw_drive_node *pos;
	char uuid_str[37];	/* uuid string is 36 chars + '\0' */
	int i;

	list_for_each_entry(pos, &drive_list, list) {

		uuid_unparse(pos->drive.id, uuid_str);
		uuid_str[36] = '\0';
		ilm_log_dbg("SCSI dev ID: %s", uuid_str);

		for (i = 0; i < pos->drive.path_num; i++) {
			ilm_log_dbg("blk_path %s", pos->drive.path[i].blk_path);
			ilm_log_dbg("sg_path %s", pos->drive.path[i].sg_path);
		}
	}
}

static int ilm_scsi_add_new_node(char *dev_node, char *sg_node, uuid_t id)
{
	struct ilm_hw_drive_node *pos, *found = NULL;
	struct ilm_hw_drive *drive;
	int ret;

	list_for_each_entry(pos, &drive_list, list) {
		ret = uuid_compare(pos->drive.id, id);
		if (!ret) {
			found = pos;
			break;
		}
	}

	if (!found) {
		found = malloc(sizeof(struct ilm_hw_drive_node));
		memset(found, 0x0, sizeof(struct ilm_hw_drive_node));
		uuid_copy(found->drive.id, id);
		list_add(&found->list, &drive_list);
	}

	drive = &found->drive;
	drive->path[drive->path_num].blk_path = strdup(dev_node);
	drive->path[drive->path_num].sg_path = strdup(sg_node);
	drive->path_num++;
	return 0;
}

int ilm_scsi_list_init(void)
{
	struct dirent **namelist;
	char devs_path[PATH_MAX];
	char dev_path[PATH_MAX];
	char blk_path[PATH_MAX];
	char dev_node[PATH_MAX];
	char sg_node[PATH_MAX];
	int i, num;
	int ret;
	char value[64];
	unsigned int maj, min;
	uuid_t uuid;

	INIT_LIST_HEAD(&drive_list);

	snprintf(devs_path, sizeof(devs_path), "%s%s",
		 SYSFS_ROOT, BUS_SCSI_DEVS);

	num = scandir(devs_path, &namelist, ilm_scsi_dir_select, NULL);
	if (num < 0) {  /* scsi mid level may not be loaded */
		ilm_log_err("Attached devices: none");
		return -1;
	}

	for (i = 0; i < num; ++i) {
		ret = snprintf(dev_path, sizeof(dev_path), "%s/%s",
			       devs_path, namelist[i]->d_name);
		if (ret < 0) {
			ilm_log_err("string is out of memory");
			goto out;
		}

		ret = snprintf(blk_path, sizeof(blk_path), "%s/%s",
			       dev_path, "block");
		if (ret < 0) {
			ilm_log_err("string is out of memory");
			goto out;
		}

		ret = ilm_scsi_find_block_node(blk_path);
		if (ret < 0)
			continue;

		snprintf(dev_node, sizeof(dev_node), "/dev/%s", blk_str);

		ret = ilm_scsi_change_sg_folder(dev_path);
		if (ret < 0) {
			ilm_log_err("fail to change sg folder");
			continue;
		}

		if (NULL == getcwd(blk_path, sizeof(blk_path))) {
			ilm_log_err("generic_dev error");
			continue;
		}

		ret = ilm_scsi_get_value(blk_path, "dev", value, sizeof(value));
		if (ret < 0) {
			ilm_log_err("fail to get device value");
			continue;
		}

		sscanf(value, "%u:%u", &maj, &min);

		ret = ilm_scsi_parse_sg_node(maj, min, sg_node);
		if (ret < 0) {
			ilm_log_err("fail to find blk node %d:%d", maj, min);
			continue;
		}

		ilm_log_err("dev_node=%s", dev_node);
		ilm_log_err("sg_node=%s", sg_node);

		ret = ilm_read_parttable_id(dev_node, &uuid);
		if (ret < 0) {
			ilm_log_err("fail to read parttable id");
			continue;
		}

		ret = ilm_scsi_add_new_node(dev_node, sg_node, uuid);
		if (ret < 0) {
			ilm_log_err("fail to add scsi node");
			goto out;
		}
	}

	ilm_scsi_dump_nodes();
out:
        for (i = 0; i < num; i++)
                free(namelist[i]);
	free(namelist);
        return 0;
}

void ilm_scsi_list_exit(void)
{
	struct ilm_hw_drive_node *pos, *next;
	struct ilm_hw_drive *drive;
	int i;

	list_for_each_entry_safe(pos, next, &drive_list, list) {
		list_del(&pos->list);

		drive = &pos->drive;
		for (i = 0; i < drive->path_num; i++) {
			free(drive->path[i].blk_path);
			free(drive->path[i].sg_path);
		}

		free(pos);
	}
}
