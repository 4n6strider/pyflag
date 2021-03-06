/*
 * The Sleuth Kit
 *
 * $Date: 2007/04/04 18:48:46 $
 *
 * Brian Carrier [carrier@sleuthkit.org]
 * Copyright (c) 2004-2005 Brian Carrier.  All rights reserved
 *
 */

#ifndef _GPT_H
#define _GPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Partition type in the safety DOS partition table */
#define GPT_PART_SOFFSET	0
#define GPT_DOS_TYPE	0xEE



/* This is located in sector 1 of the disk */
#define GPT_HEAD_OFFSET	1
#define GPT_HEAD_SIG	0x5452415020494645ULL

    typedef struct {
        uint8_t signature[8];   /* EFI PART */
        uint8_t version[4];
        uint8_t head_size_b[4]; /* size of partition header */
        uint8_t head_crc[4];    /* crc of header */
        uint8_t f1[4];
        uint8_t head_lba[8];    /* lba of this header */
        uint8_t head2_lba[8];   /* lba of second header */
        uint8_t partarea_start[8];      /* lba of partition area start */
        uint8_t partarea_end[8];        /* lba of partition area end */
        uint8_t guid[16];       /* disk GUID */
        uint8_t tab_start_lba[8];       /* lba of table start */
        uint8_t tab_num_ent[4]; /* num of table entries */
        uint8_t tab_size_b[4];  /* size of each table entry */
        uint8_t tab_crc[4];     /* crc of table */
        uint8_t f2[420];
    } gpt_head;


/* The location of this is specified in the header - tab_start */
    typedef struct {
        uint8_t type_guid[16];  /* partition type guid */
        uint8_t id_guid[16];    /* unique partition GUID */
        uint8_t start_lba[8];   /* Starting lba of part */
        uint8_t end_lba[8];     /* end lba of part */
        uint8_t flags[8];       /* flags */
        uint8_t name[72];       /* name in unicode */
    } gpt_entry;


#ifdef __cplusplus
}
#endif
#endif
