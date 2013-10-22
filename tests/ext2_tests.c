#include "minunit.h"
#include <errno.h>
#include <ext2.h>

char *test_ext2_load()
{
  image_t *im = image_load("tests/testimg.img");
  partition_t *p = partition_open(im, 0);

  fs_t *fs = fs_load(p, ext2);

  ext2_data_t *d = fs->data;
  mu_assert(d->num_groups == 3, "Wrong number of groups");

  fs_close(fs);
  partition_close(p);
  image_close(im);

  return NULL;
}

char *test_ext2_readdir()
{
  image_t *im = image_load("tests/testimg.img");
  partition_t *p = partition_open(im, 0);
  fs_t *fs = fs_load(p, ext2);

  INODE i = fs_find(fs, "/");
  dirent_t *de;
  de = fs_readdir(fs, i, 0);
  mu_assert(!strcmp(de->name, "."), "Directory listing is wrong");
  free(de);
  de = fs_readdir(fs, i, 1);
  mu_assert(!strcmp(de->name, ".."), "Directory listing is wrong");
  free(de);
  de = fs_readdir(fs, i, 2);
  mu_assert(!strcmp(de->name, "lost+found"), "Directory listing is wrong");
  free(de);

  fs_close(fs);
  partition_close(p);
  image_close(im);
  return NULL;
}


char *all_tests() {
  mu_suite_start();
  mu_run_test(test_ext2_load);
  mu_run_test(test_ext2_readdir);
  return NULL;
}

RUN_TESTS(all_tests);
