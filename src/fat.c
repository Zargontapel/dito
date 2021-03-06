#include "fat.h"
#include "fs.h"
#include "image.h"
#include <dito.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

fs_driver_t fat_driver = {
  fat_read,
  fat_write,
  fat_touch,
  fat_readdir,
  fat_link,
  fat_unlink,
  fat_fstat,
  fat_mkdir,
  fat_rmdir,
  1,
  fat_hook_load,
  fat_hook_create,
  fat_hook_close,
  fat_hook_check
};

int fat_bits(struct fs_st *fs)
{
  // Return values:
  // 12: FAT12
  // 16: FAT16
  // 32: FAT32

  if(!fs)
    return 0;

  if(fat_num_clusters(fs) < 4085)
    return 12; // FAT12
  if(fat_num_clusters(fs) < 65525)
    return 16; // FAT16
  return 32; // FAT32
}

size_t fat_readclusters(struct fs_st *fs, void *buffer, size_t cluster, size_t length)
{
  // cluster = 0 will read root directory for fat12/16
  // cluster >= 2 will read actual data clusters
  //
  // Reading of root directory cannot be resumed. I.e. reading, for
  // example, only the third cluster of the root directory is not
  // possible.

  if(!fs)
    return 0;
  if(!buffer)
    return 0;
  if(!length)
    return 0;

  size_t start = fat_first_data_sector(fs);
  if(cluster >= 2)
  {
    start += fat_root_sectors(fs);
    start += (cluster-2)*fat_bpb(fs)->sectors_per_cluster;
  }
  length *= fat_bpb(fs)->sectors_per_cluster;

  return partition_readblocks(fs->p, buffer, start, length);
}

size_t fat_writeclusters(struct fs_st *fs, void *buffer, size_t cluster, size_t length)
{
  // See comments in fat_redcluster() re: reading root directory
  // clusters.

  if(!fs)
    return 0;
  if(!buffer)
    return 0;
  if(!length)
    return 0;

  size_t start = fat_first_data_sector(fs);
  if(cluster >= 2)
  {
    start += fat_root_sectors(fs);
    start += (cluster-2)*fat_bpb(fs)->sectors_per_cluster;
  }
  length *= fat_bpb(fs)->sectors_per_cluster;

  return partition_writeblocks(fs->p, buffer, start, length);
}

uint32_t fat_read_fat(struct fs_st *fs, uint32_t cluster)
{
  if(!fs)
    return 0;

  // Only implemented for FAT12 right now.
  if(fat_bits(fs) != 12)
    return 0;

  uint32_t offset = cluster + cluster/2;
  uint16_t value = *(uint16_t *)&(fat_data(fs)->fat[offset]);
  if (cluster & 0x0001)
    value >>= 4;
  else
    value &= 0x0FFF;
  return value;
}

void fat_write_fat(struct fs_st *fs, uint32_t cluster, uint32_t set)
{
  if(!fs)
    return;

  // Only implemented for FAT12 right now.
  if(fat_bits(fs) != 12)
    return;

  uint32_t offset = cluster + cluster/2;
  uint16_t value = *(uint16_t *)&(fat_data(fs)->fat[offset]);
  if(cluster & 0x0001)
    value = (value & 0x000F) | (set << 4);
  else
    value = (value & 0xF000) | (set & 0x0FFF);
  *(uint16_t *)&(fat_data(fs)->fat[offset]) = value;
}

uint32_t fat_find_free(struct fs_st *fs)
{
  if(!fs)
    return 0;

  uint32_t i = 3; // Skip first two entries
  while(i < fat_num_clusters(fs))
  {
    if(fat_read_fat(fs, i))
      i++;
    else
      return i;
  }

  return 0;
}

fat_inode_t *fat_get_inode(struct fs_st *fs, INODE ino)
{
  if(!fs)
    return 0;
  if(!ino)
    return 0;

  ino--;
  fat_inode_t *ret = fat_data(fs)->inodes;
  while(ino)
  {
    ret = ret->next;
    ino--;
    if(!ret)
      return 0;
  }
  return ret;
}

uint32_t fat_clustercount(struct fs_st *fs, INODE ino)
{
  if(!fs)
    return 0;
  if(!ino)
    return 0;

  if(ino == 1) // Special case for root directory (FAT12/16)
  {
    return fat_bpb(fs)->root_count*32/fat_clustersize(fs);
  }

  uint32_t ret = 0;
  fat_inode_t *inode = fat_get_inode(fs, ino);
  uint32_t cluster = inode->cluster;
  while(cluster < FAT_END)
  {
    ret++;
    cluster = fat_read_fat(fs, cluster);
  }
  return ret;
}

uint32_t *fat_get_clusters(struct fs_st *fs, INODE ino)
{
  if(!fs)
    return 0;
  if(!ino)
    return 0;

  uint32_t c_count = fat_clustercount(fs, ino);
  uint32_t *clusters = calloc(c_count + 1, sizeof(uint32_t));

  uint32_t i = 0;
  if(ino == 1)
  {
    while(i < c_count)
    {
      clusters[i] = i;
      i++;
    }
  } else {
    fat_inode_t *inode = fat_get_inode(fs, ino);
    uint32_t cluster = inode->cluster;
    while(i < c_count)
    {
      clusters[i] = cluster;
      cluster = fat_read_fat(fs, cluster);
      i++;
    }
  }
  return clusters;
}

char *fat_read_longname(void *de)
{
  if(!de)
    return 0;

  fat_longname_t *ln = de;
  if(ln[0].attrib != FAT_DIR_LONGNAME)
    return 0;
  if(!(ln[0].num & 0x40))
    return 0;

  int entries = ln[0].num & 0x1F;
  char *name = calloc(entries*13 + 1, 1);
  int i = 0;
  int j = entries;
  while(j)
  {
    j--;
    name[i++] = ln[j].name1[0];
    name[i++] = ln[j].name1[2];
    name[i++] = ln[j].name1[4];
    name[i++] = ln[j].name1[6];
    name[i++] = ln[j].name1[8];
    name[i++] = ln[j].name2[0];
    name[i++] = ln[j].name2[2];
    name[i++] = ln[j].name2[4];
    name[i++] = ln[j].name2[6];
    name[i++] = ln[j].name2[8];
    name[i++] = ln[j].name2[10];
    name[i++] = ln[j].name3[0];
    name[i++] = ln[j].name3[2];
  }

  return name;
}

uint8_t fat_checksum(const char *shortname)
{
  if(!shortname)
    return 0;

  int i;
  uint8_t sum = 0;
  for(i = 11; i; i--)
    sum = ((sum & 1) << 7) + (sum >> 1) + *shortname++;

  return sum;
}

char *fat_make_shortname(const char *longname)
{
  if(!longname)
    return 0;

  char *shortname = calloc(11, 1);

  // Copy first 8 characters
  strncpy(shortname, longname, 8);
  int i = 0;

  // Break string at the first .
  char *dot =strchr(shortname, '.');
  if(dot)
  {
    dot[0] = '\0';
  }

  // Pad with spaces
  i = strlen(shortname);
  while(i < 8)
    shortname[i++] = ' ';

  // Find and copy extension
  dot = strrchr(longname, '.');
  if(dot)
    strncpy(&shortname[8], &dot[1], 3);

  // Pad with spaces
  i = strlen(shortname);
  while(i < 11)
    shortname[i++] = ' ';

  return shortname;
}

void *fat_write_longname(void *de, const char *name)
{
  if(!de)
    return 0;
  if(!name)
    return 0;

  char *shortname = fat_make_shortname(name);

  size_t entries = strlen(name)/13; // 13 characters per entry
  if(strlen(name)%13) entries++;

  // Convert chars to UTF-16
  char *buffer = calloc(entries, 26);
  size_t i = 0, j = 0;
  while(i < strlen(name))
  {
    buffer[j] = name[i];
    i++;
    j += 2;
  }
  j += 2;
  while(j < entries*26)
  {
    buffer[j++] = '\xff';
  }

  fat_longname_t *ln = de;

  i = entries;
  j = 1;
  int k = 0;
  while(i)
  {
    i--;
    ln[i].attrib = FAT_DIR_LONGNAME;
    ln[i].num = j++;
    memcpy(ln[i].name1, &buffer[k], 10);
    k += 10;
    memcpy(ln[i].name2, &buffer[k], 12);
    k += 12;
    memcpy(ln[i].name3, &buffer[k], 4);
    k += 4;
    ln[i].entry_type = 0;
    ln[i].checksum = fat_checksum(shortname);
  }
  ln[0].num |= 0x40;

  free(buffer);
  free(shortname);

  // Return where the actual directory entry should start
  return &ln[entries];
}



int fat_read(struct fs_st *fs, INODE ino, void *buffer, size_t length, size_t offset)
{
  if(!fs)
    return 0;
  if(!ino)
    return 0;
  if(!buffer)
    return 0;
  if(!length)
    return 0;

  fat_inode_t *inode = fat_get_inode(fs, ino);
  uint32_t *clusters = fat_get_clusters(fs, ino);
  uint32_t size = inode->size;
  if(!size) // size=0 ==> Probably a directory
    size = fat_clustercount(fs, ino)*fat_clustersize(fs);

  if(offset + length > size)
    length = size - offset;

  uint32_t start_cluster = offset/(fat_clustersize(fs));
  uint32_t cluster_offset = offset%(fat_clustersize(fs));
  uint32_t num_clusters = (length+cluster_offset)/(fat_clustersize(fs));
  if((length+cluster_offset)%(fat_clustersize(fs)))
    num_clusters++;

  // This can be optimized memory-wise.
  void *buff = 0;
  void *b = buff = calloc(1, num_clusters*fat_clustersize(fs));
  uint32_t i = start_cluster;
  while( i < (start_cluster + num_clusters))
  {
    fat_readclusters(fs, b, clusters[i], 1);
    b = (void *)((size_t)b + fat_clustersize(fs));
    i++;
  }

  memcpy(buffer, (void *)((size_t)buff + cluster_offset), length);

  free(clusters);
  free(buff);
  return length;
}

int fat_write(struct fs_st *fs, INODE ino, void *buffer, size_t length, size_t offset)
{
  if(!fs)
    return 0;
  if(!ino)
    return 0;
  if(!buffer)
    return 0;

  fat_inode_t *inode = fat_get_inode(fs, ino);
  uint32_t *clusters = fat_get_clusters(fs, ino);
  uint32_t size = inode->size;
  if(!size)
    size = fat_clustercount(fs, ino)*fat_clustersize(fs);

  if(offset + length > size)
    length = size - offset;

  uint32_t start_cluster = offset/(fat_clustersize(fs));
  uint32_t cluster_offset = offset%(fat_clustersize(fs));
  uint32_t num_clusters = (length+cluster_offset)/(fat_clustersize(fs));
  if((length+cluster_offset)%(fat_clustersize(fs)))
    num_clusters++;

  void *buff = 0;
  void *b = buff = calloc(1, num_clusters*fat_clustersize(fs));
  fat_read(fs, ino, buff, num_clusters*fat_clustersize(fs), offset - cluster_offset);
  memcpy((void *)((size_t)buff + cluster_offset), buffer, length);
  uint32_t i = start_cluster;
  while( i < (start_cluster + num_clusters))
  {
    fat_writeclusters(fs, b, clusters[i], 1);
    b = (void *)((size_t)b + fat_clustersize(fs));
    i++;
  }

  free(clusters);
  free(buff);
  return length;
}

INODE fat_touch(struct fs_st *fs, fstat_t *st)
{
  if(!fs)
    return 0;
  if(!st)
    return 0;

  // Create inode
  INODE ret = fat_data(fs)->next++;
  fat_inode_t *ino = calloc(1, sizeof(fat_inode_t));

  ino->parent = -1;
  if((st->mode & S_DIR) == S_DIR)
    ino->type = FAT_DIR_DIRECTORY;
  ino->atime = st->atime;
  ino->ctime = st->ctime;
  ino->mtime = st->mtime;
  ino->size = st->size;

  // Allocate clusters
  int32_t size = ino->size - fat_clustersize(fs);
  uint32_t current = ino->cluster = fat_find_free(fs);
  fat_write_fat(fs, current, FAT_END);
  while(size >0)
  {
    fat_write_fat(fs, current, fat_find_free(fs));
    current = fat_read_fat(fs, current);
    fat_write_fat(fs, current, FAT_END);
    if(fat_clustersize(fs) > size)
      size = 0;
    else
      size -= fat_clustersize(fs);
  }

  fat_data(fs)->last->next = ino;
  fat_data(fs)->last = ino;

  return ret;
}

dirent_t *fat_readdir(struct fs_st *fs, INODE dir, unsigned int num)
{
  // Since FAT doesn't use inodes but stores all metadata in directory
  // entries, the inodes doesn't "exits" until they have been found
  // through readdir()

  if(!fs)
    return 0;
  if(!dir)
    return 0;

  fat_inode_t *dir_ino = 0;
  if(!(dir_ino = fat_get_inode(fs, dir)))
    return 0;
  if(dir_ino->type != FAT_DIR_DIRECTORY)
    return 0;

  if(num == 0) // .
  {
    dirent_t *ret = calloc(1, sizeof(dirent_t));
    ret->name = strdup(".");
    ret->ino = dir;
    return ret;
  } else if(num == 1) { // ..
    dirent_t *ret = calloc(1, sizeof(dirent_t));
    ret->name = strdup("..");
    ret->ino = dir_ino->parent;
    return ret;
  } else { // Other entries

    if(dir != 1)
      num +=2; // Skip . and ..

    // Read directory entries
    uint32_t size = fat_clustercount(fs, dir)*fat_clustersize(fs);
    void *buffer = calloc(1, size);
    size_t max = (size_t)buffer + size;
    fat_read(fs, dir, buffer, size, 0);

    fat_dir_t *de = buffer;
    while((num > 2) && ((size_t)de < max))
    {
      if(de->name[0] == 0)
      {
        // Encountered last entry
        free(buffer);
        return 0;
      }
      if(de->attrib == FAT_DIR_LONGNAME || de->name[0] == 0xE5)
      {
        // Skip longname entry or deleted entries
        de++;
      } else {
        de++;
        num--;
      }
    }
    if(de->name[0] == 0 || ((size_t)de >= max))
    {
      free(buffer);
      return 0;
    }

    // Read longname and skip longname entries
    char *longname = fat_read_longname(de);
    while(de->attrib == FAT_DIR_LONGNAME) de++;

    // Now de is the entry we want
    dirent_t *ret = calloc(1, sizeof(dirent_t));
    ret->ino = fat_data(fs)->next;
    if(longname)
    {
      ret->name = strdup(longname);
      free(longname);
    } else {
      // Parse 8.3 name
      ret->name = calloc(8, 1);
      char *c;
      if( (c = strchr((char *)de->name, ' ')))
        c[0] = '\0';
      c = stpncpy(ret->name, (char *)de->name, 8);
      if(de->attrib != FAT_DIR_DIRECTORY)
      {
        c[0] = '.';
        c++;
      }
        strncpy(c, (char *)&de->name[8],3);
        if((c = strchr(ret->name, ' ')))
          c[0] = '\0';
    }

    // Build inode
    fat_inode_t *inode = calloc(1, sizeof(fat_inode_t));
    inode->parent = dir;
    inode->type = de->attrib;
    inode->cluster = (de->cluster_high << 16) + de->cluster_low;
    inode->size = de->size;
    // Parse times
    struct tm *atime = calloc(1, sizeof(struct tm));
    atime->tm_mday = (de->adate & 0x1F);
    atime->tm_mon = ((de->adate >> 5) & 0xF);
    atime->tm_year = ((de->adate >> 9) & 0x7F);

    /* struct tm atime = */ 
    /* { */
    /*   0, 0, 0, //sec, min, hour */
    /*   (de->adate & 0x1F), // day */
    /*   ((de->adate >> 5) & 0xF), // month */
    /*   ((de->adate >> 9) & 0x7F), // year */
    /*   0,0,0,0,0 */
    /* }; */

    struct tm *ctime = calloc(1, sizeof(struct tm));
    ctime->tm_sec = (de->ctime & 0x1F);
    ctime->tm_min = ((de->ctime >> 5) & 0x3F);
    ctime->tm_hour = ((de->ctime >> 11) & 0x1F);
    ctime->tm_mday = (de->cdate & 0x1F);
    ctime->tm_mon = ((de->cdate >> 5) & 0xF);
    ctime->tm_year = ((de->cdate >> 9) & 0x7F);

    /* struct tm ctime = */ 
    /* { */
    /*   (de->ctime & 0x1F), // sec */
    /*   ((de->ctime >> 5) & 0x3F), // min */
    /*   ((de->ctime >> 11) & 0x1F), // hour */
    /*   (de->cdate & 0x1F), // day */
    /*   ((de->cdate >> 5) & 0xF), // month */
    /*   ((de->cdate >> 9) & 0x7F), // year */
    /*   0,0,0,0,0 */
    /* }; */

    struct tm *mtime = calloc(1, sizeof(struct tm));
    mtime->tm_sec = (de->mtime & 0x1F);
    mtime->tm_min = ((de->mtime >> 5) & 0x3F);
    mtime->tm_hour = ((de->mtime >> 11) & 0x1F);
    mtime->tm_mday = (de->mdate & 0x1F);
    mtime->tm_mon = ((de->mdate >> 5) & 0xF);
    mtime->tm_year = ((de->mdate >> 9) & 0x7F);

    /* struct tm mtime = */ 
    /* { */
    /*   (de->mtime & 0x1F), // sec */
    /*   ((de->mtime >> 5) & 0x3F), // min */
    /*   ((de->mtime >> 11) & 0x1F), // hour */
    /*   (de->mdate & 0x1F), // day */
    /*   ((de->mdate >> 5) & 0xF), // month */
    /*   ((de->mdate >> 9) & 0x7F), // year */
    /*   0,0,0,0,0 */
    /* }; */
    inode->atime = mktime(atime);
    inode->ctime = mktime(ctime);
    inode->mtime = mktime(mtime);
    free(atime);
    free(ctime);
    free(mtime);

    // Insert new inode into list
    fat_data(fs)->last->next = inode;
    fat_data(fs)->last = inode;
    fat_data(fs)->next++;

    free(buffer);
    return ret;

  }
}

int fat_link(struct fs_st *fs, INODE ino, INODE dir, const char *name)
{
  if(!fs)
    return 1;
  if(!ino)
    return 1;
  if(!dir)
    return 1;
  if(!name)
    return 1;

  fat_inode_t *dino = fat_get_inode(fs, dir);
  fat_inode_t *iino = fat_get_inode(fs, ino);
  iino->parent = dir;
  uint32_t size = fat_clustercount(fs, dir)*fat_clustersize(fs);
  void *buffer = calloc(1, size + fat_clustersize(fs));
  fat_read(fs, dir, buffer, size, 0);

  uint32_t entries = strlen(name)/13 + 1; // Including longname
  uint32_t found = 0;
  fat_dir_t *firstfree = 0;
  fat_dir_t *de = buffer;
  int counter = 0;
  while(de->name[0] != 0)
  {
    if(de->name[0] == 0xE5)
    {
      if(!found)
        firstfree = de;
      found++;
      if(found == entries)
      {
        // Found a big enough hole
        de = firstfree;
        break;
      }
    } else {
      found = 0;
    }
    de++;
    counter++;
  }

  firstfree = de;
  // de is the first free entry.
  if(strcmp(name, ".          ") && strcmp(name, "..         "))
  {
    de = fat_write_longname(de, name);
    char *shortname = fat_make_shortname(name);
    strncpy((char *)de->name, shortname, 11);
    free(shortname);
  } else {
    // . and .. shouldn't have longnames
    strcpy((char *)de->name, name);
  }
  de->attrib = iino->type;
  de->csec = 0;

  // Wibbly-wobbly timey-wimey stuff
  time_t ctm = iino->ctime;
  struct tm *ctime = gmtime(&ctm);
  de->ctime = ((ctime->tm_hour & 0x1F) << 11) | ((ctime->tm_min &0x3F) << 5) | ((ctime->tm_sec & 0x1F));
  de->cdate = ((ctime->tm_year & 0x7F) << 9) | ((ctime->tm_mon & 0xF) << 5) | ((ctime->tm_mday & 0x1F));
  time_t atm = iino->atime;
  struct tm *atime = gmtime(&atm);
  de->adate = ((atime->tm_year & 0x7F) << 9) | ((atime->tm_mon & 0xF) << 5) | ((atime->tm_mday & 0x1F));
  time_t mtm = iino->mtime;
  struct tm *mtime = gmtime(&mtm);
  de->mtime = ((mtime->tm_hour & 0x1F) << 11) | ((mtime->tm_min &0x3F) << 5) | ((mtime->tm_sec & 0x1F));
  de->mdate = ((mtime->tm_year & 0x7F) << 9) | ((mtime->tm_mon & 0xF) << 5) | ((mtime->tm_mday & 0x1F));

  de->cluster_high = iino->cluster >> 16;
  de->cluster_low = iino->cluster & 0xFF;
  de->size = iino->size;

  // Increase size for directory if needed
  de++;
  if((size_t)de > (size_t)buffer + size)
  {
    uint32_t last = 0, current = dino->cluster;
    while(current != FAT_END)
    {
      last = current;
      current = fat_read_fat(fs, current);
    }
    current = fat_find_free(fs);
    fat_write_fat(fs, last, current);
    fat_write_fat(fs, current, FAT_END);
    size += fat_clustersize(fs);
  }
  fat_write(fs, dir, buffer, size, 0);

  free(buffer);

  return 0;
}

int fat_unlink(struct fs_st *fs, INODE dir, unsigned int num)
{
  if(!fs)
    return 1;
  if(!dir)
    return 1;
  if(num < 2)
    return 1;

  fat_inode_t *dir_ino = 0;
  if(!(dir_ino = fat_get_inode(fs, dir)))
    return 1;
  if(dir_ino->type != FAT_DIR_DIRECTORY)
    return 1;

  dirent_t *dirent = fat_readdir(fs, dir, num);
  INODE item = dirent->ino;
  free(dirent->name);
  free(dirent);

  uint32_t size = fat_clustercount(fs, dir)*fat_clustersize(fs);
  void *buffer = calloc(1, size);
  size_t max = (size_t)buffer + size;
  fat_read(fs, dir, buffer, size, 0);

  if(dir != 1)
    num +=2;

  // Find the right entry
  fat_dir_t *de = buffer;
  while((num > 2) && ((size_t)de < max))
  {
    if(de->name[0] == 0)
    {
      free(buffer);
      return 1;
    }
    if(de->attrib == FAT_DIR_LONGNAME)
    {
      de++;
    } else {
      de++;
      num--;
    }
  }
  if(de->name[0] == 0)
  {
    free(buffer);
    return 1;
  }


  void *start = de;

  // Find start of next entry
  while(de->attrib == FAT_DIR_LONGNAME) de++;
  de++;
  void *next = de;
  void *buffer2 = calloc(1, size);
  // Copy data, skipping the entries we want to remove.
  memcpy(buffer2, buffer, (size_t)start - (size_t)buffer);
  memcpy((void *)((size_t)buffer2 + ((size_t)start - (size_t)buffer)), next, (size_t)max - (size_t)next);
  // Write it back
  fat_write(fs, dir, buffer2, size, 0);

  free(buffer);
  free(buffer2);

  // Mark the files clusters as free in the FAT
  uint32_t *clusters = fat_get_clusters(fs, item);
  int i = 0;
  while(clusters[i])
  {
    fat_write_fat(fs, clusters[i], 0);
    i++;
  }

  free(clusters);

  return 0;
}

fstat_t *fat_fstat(struct fs_st *fs, INODE ino)
{
  if(!fs)
    return 0;
  if(!ino)
    return 0;
  fat_inode_t *inode = fat_get_inode(fs, ino);
  if(!inode)
    return 0;

  fstat_t *ret = calloc(1, sizeof(fstat_t));
  ret->size = inode->size;
  if(inode->type == FAT_DIR_DIRECTORY)
    ret->mode = S_DIR;
  ret->mode |= 0777;
  ret->atime = inode->atime;
  ret->ctime = inode->ctime;
  ret->mtime = inode->mtime;

  return ret;
}

int fat_mkdir(struct fs_st *fs, INODE parent, const char *name)
{
  if(!fs)
    return 1;
  if(!parent)
    return 1;
  if(!name)
    return 1;

  fstat_t st;
  st.size = 0;
  st.mode = S_DIR | 0755;
  st.atime = time(0);
  st.ctime = time(0);
  st.mtime = time(0);

  INODE child = fat_touch(fs, &st);
  if(fat_link(fs, child, parent, name))
    return 1;

  fat_dir_t *de = calloc(1, fat_clustersize(fs));
  fat_write(fs, child, de, fat_clustersize(fs), 0);
  free(de);

  fat_link(fs, child, child, ".          ");
  fat_link(fs, parent, child, "..         ");

  return 0;
}

int fat_rmdir(struct fs_st *fs, INODE dir, unsigned int num)
{
  if(!fs)
    return 1;
  if(!dir)
    return 1;

  dirent_t *de = fat_readdir(fs, dir, num);
  INODE target = de->ino;
  free(de->name);
  free(de);

  if(fat_readdir(fs, target, 2))
    return 1; // Not empty

  fat_unlink(fs, dir, num);

  return 0;
}

void *fat_hook_load(struct fs_st *fs)
{
  fat_data_t *data = fs->data = calloc(1, sizeof(fat_data_t));

  // Read BPB
  data->bpb = calloc(1, BLOCK_SIZE);
  partition_readblocks(fs->p, data->bpb, 0, 1);

  // Read FAT
  data->fat = calloc(fat_bpb(fs)->sectors_per_fat, BLOCK_SIZE);
  partition_readblocks(fs->p, data->fat, data->bpb->reserved_sectors, fat_bpb(fs)->sectors_per_fat);

  // Generate root inode
  data->inodes = calloc(1, sizeof(fat_inode_t));
  data->inodes->parent = 1;
  data->inodes->type = FAT_DIR_DIRECTORY;
  data->inodes->cluster = 0;
  data->inodes->size = 0;
  data->last = data->inodes;
  data->next = 2;

  return 0;
}

void *fat_hook_create(struct fs_st *fs)
{
  fat_data_t *data = fs->data = calloc(1, sizeof(fat_data_t));

  fat_bpb_t *bpb = data->bpb = calloc(1, sizeof(fat_bpb_t));

  uint32_t num_sectors = fs->p->length;
  uint32_t fs_size = num_sectors * BLOCK_SIZE;

  // What kind of FAT do we need?
  int fat_bits = 12;
  if(fs_size >= 0x80000000) // 2048 Mb
    fat_bits = 32;
  else if(fs_size >= 0x1000000) // 16 Mb
    fat_bits = 16;

  // All but FAT12 are unsupported
  if(fat_bits != 12)
  {
    printf("Warning: Partition size requires fat 16 or 32, which are not implemented!\n");
    return 0;
  }

  printf("Formating using FAT%d\n", fat_bits);

  int cluster_size = 8;
  while(fs_size >= 0x1000000) // 16 Mb
  {
    cluster_size *= 2;
    fs_size /= 2;
  }

  // Set up boot parameter block
  bpb->jmp[0] = 0xEB;
  bpb->jmp[1] = 0x3C;
  bpb->jmp[2] = 0x90;
  strncpy((char *)bpb->identifier, "mkdosfs ",8);
  bpb->bytes_per_sector = 512;
  bpb->sectors_per_cluster = cluster_size;
  bpb->reserved_sectors = (fat_bits == 32)?32:4;
  bpb->fat_count = 2;
  if(fat_bits != 32)
  {
    if(fs_size > 0x400000) // 4 mb (2.88 mb floppy disk - I think...)
      bpb->root_count = 512;
    else
      bpb->root_count = 240;
  } else {
    bpb->root_count = 0;
  }
  bpb->total_sectors_small = (num_sectors > 65535)?0:num_sectors;
  bpb->total_sectors_large = (num_sectors > 65535)?num_sectors:0;
  bpb->media_descriptor = (fs_size > 0x400000)?0xF8:0xF0;
  if(fat_bits != 32)
  {
    uint32_t fat_size = num_sectors/cluster_size - bpb->reserved_sectors;
    uint32_t entries_per_sector = bpb->bytes_per_sector*8/fat_bits;
    bpb->sectors_per_fat = fat_size/entries_per_sector;
    if(fat_size%entries_per_sector)
      bpb->sectors_per_fat++;
  } else {
    bpb->sectors_per_fat = 0;
  }
  bpb->sectors_per_track = 32;
  bpb->num_heads = 64;
  bpb->hidden_sectors = 0;

  // Set up FAT tables
  data->fat = calloc(fat_bpb(fs)->sectors_per_fat, BLOCK_SIZE);
  fat_write_fat(fs, 0, 0xF00 | bpb->media_descriptor);
  fat_write_fat(fs, 1, 0xFFF);

  // Write boot parameter block to disk
  partition_writeblocks(fs->p, data->bpb, 0, 1);

  return 0;
}

void fat_hook_close(struct fs_st *fs)
{
  if(!fs)
    return;

  // Write FATs to disk
  fat_data_t *data = fat_data(fs);
  int i = 0;
  uint32_t offset = data->bpb->reserved_sectors;
  while(i < data->bpb->fat_count)
  {
    partition_writeblocks(fs->p, data->fat, offset,  data->bpb->sectors_per_fat);
    offset += data->bpb->sectors_per_fat;
    i++;
  }

  // Free buffered inodes
  while(data->inodes)
  {
    data->last = data->inodes;
    data->inodes = data->inodes->next;
    free(data->last);
  }

  free(data->bpb);
  free(data->fat);
  free(data);
  return;
}

int fat_hook_check(struct fs_st *fs)
{
  if(!fs)
    return 0;
  return 0;
}

