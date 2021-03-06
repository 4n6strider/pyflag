/* Sleuthkit python module for use by pyflag
 * Contains two new types (skfs and skfile).
 * skfs implement an interface somewhat similar to python's 'os' module.
 * skfile implements a file-like object for accessing files through sk.
 */

#include <Python.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sk.h"
#include "list.h"
#include "misc.h"
#include "talloc.h"

#include "tsk/libtsk.h"
#include "tsk/fs/tsk_ntfs.h"

#ifdef __MINGW32__
// This does not make sense on windows
#define S_ISLNK(x) 0
#endif

//#define SK_TALLOC_HACK 0

static int TSK_IMG_INFO_TYPE_PYFILE_TYPE	=	0x40;

/** Some accounting for tracking down memleaks **/
static int skfs_object_count=0;
static int skfile_object_count=0;

/*
 * Suggested functions:
 * skfs:
 * lookup (return inode for a path)
 * walk (same as os.walk)
 * stat (return stat info for an path)
 * isdir (is this a dir)
 * islink (is this a link)
 * isfile (is this a file)
 * skfile:
 * read (read from path)
 */

/** 
PyFlag uses a modified sleuthkit which uses talloc. This allows us to
control memroy leaks within pf. Since sleuthkit is a single shot tool
- memory leaks are probably not too important. When used within PyFlag
it is critical that we dont leak memory. Therefore talloc is essential.
*/

// This is used to fix tsk_error_get's stupid behaviour of returning
// null when no error occured.
char *error_get() {
  const char *result = tsk_error_get();

  if(!result) result="";

  return (char *)result;
};

static TSK_WALK_RET_ENUM
inode_walk_callback(TSK_FS_INFO *fs, TSK_FS_INODE *fs_inode, void *ptr) {
    PyObject *inode, *list;
    list = (PyObject *)ptr;
    
    // add each ntfs data attribute
    if (((fs->ftype & TSK_FS_INFO_TYPE_FS_MASK) == TSK_FS_INFO_TYPE_NTFS_TYPE) && (fs_inode)) {
        TSK_FS_DATA *fs_data;

        for(fs_data = fs_inode->attr; fs_data; fs_data = fs_data->next) {
            if(!(fs_data->flags & TSK_FS_DATA_INUSE))
                continue;

            if(fs_data->type == NTFS_ATYPE_DATA) {
                inode = (PyObject *)PyObject_New(skfs_inode, &skfs_inodeType);
                ((skfs_inode *)inode)->inode = fs_inode->addr;
                ((skfs_inode *)inode)->type = fs_data->type;
                ((skfs_inode *)inode)->id = fs_data->id;
                ((skfs_inode *)inode)->alloc = (fs_inode->flags & TSK_FS_INODE_FLAG_ALLOC) ? 1 : 0;

                PyList_Append(list, inode);
                Py_DECREF(inode);
            }
        }
    } else {
        // regular filesystems dont have type-id, make them 0
        inode = (PyObject *)PyObject_New(skfs_inode, &skfs_inodeType);
        ((skfs_inode *)inode)->inode = fs_inode->addr;
        ((skfs_inode *)inode)->type = 0;
        ((skfs_inode *)inode)->id = 0;
        ((skfs_inode *)inode)->alloc = (fs_inode->flags & TSK_FS_INODE_FLAG_ALLOC) ? 1 : 0;

        PyList_Append(list, inode);
        Py_DECREF(inode);
    }
	return TSK_WALK_CONT;
}

/* Here are a bunch of callbacks and helpers used to give sk a more filesystem
 * like interface */

/* add this to the list, called by the callback */
void listdent_add_dent(TSK_FS_DENT *fs_dent, TSK_FS_DATA *fs_data, struct dentwalk *dentlist) {
    struct dentwalk *p = talloc(dentlist, struct dentwalk);

    p->path = talloc_strndup(p, fs_dent->name, fs_dent->name_max - 1);

    p->type = p->id = 0;
    if(fs_data) {
        p->type = fs_data->type;
        p->id = fs_data->id;
    }

    /* print the data stream name if it exists and is not the default NTFS */
    if ((fs_data) && (((fs_data->type == NTFS_ATYPE_DATA) &&
        (strcmp(fs_data->name, "$Data") != 0)) ||
        ((fs_data->type == NTFS_ATYPE_IDXROOT) &&
        (strcmp(fs_data->name, "$I30") != 0)))) {
        p->path = talloc_asprintf_append(p->path, ":%s", fs_data->name);
    } 

    p->alloc = 1; // allocated
    if(fs_dent->flags & TSK_FS_DENT_FLAG_UNALLOC) {
        if((fs_dent->fsi) && (fs_dent->fsi->flags & TSK_FS_INODE_FLAG_ALLOC))
            p->alloc = 2; // realloc
        else
            p->alloc = 0; // unalloc
    }

    p->inode = fs_dent->inode;
    p->ent_type = fs_dent->ent_type;

    list_add_tail(&p->list, &dentlist->list);
}

/* 
 * call back action function for dent_walk
 * This is a based on the callback in fls_lib.c, it adds an entry for each
 * named ADS in NTFS
 */
static TSK_WALK_RET_ENUM
listdent_walk_callback_dent(TSK_FS_INFO * fs, TSK_FS_DENT * fs_dent, void *ptr) {
    struct dentwalk *dentlist = (struct dentwalk *)ptr;

	/* Make a special case for NTFS so we can identify all of the
	 * alternate data streams!
	 */
    if (((fs->ftype & TSK_FS_INFO_TYPE_FS_MASK) == TSK_FS_INFO_TYPE_NTFS_TYPE) && (fs_dent->fsi)) {

        TSK_FS_DATA *fs_data = fs_dent->fsi->attr;
        uint8_t printed = 0;

        while ((fs_data) && (fs_data->flags & TSK_FS_DATA_INUSE)) {

            if (fs_data->type == NTFS_ATYPE_DATA) {
                mode_t mode = fs_dent->fsi->mode;
                uint8_t ent_type = fs_dent->ent_type;

                printed = 1;

                /* 
                * A directory can have a Data stream, in which
                * case it would be printed with modes of a
                * directory, although it is really a file
                * So, to avoid confusion we will set the modes
                * to a file so it is printed that way.  The
                * entry for the directory itself will still be
                * printed as a directory
                */

                if ((fs_dent->fsi->mode & TSK_FS_INODE_MODE_FMT) == TSK_FS_INODE_MODE_DIR) {

                    /* we don't want to print the ..:blah stream if
                    * the -a flag was not given
                    */
                    if ((fs_dent->name[0] == '.') && (fs_dent->name[1])
                        && (fs_dent->name[2] == '\0')) {
                        fs_data = fs_data->next;
                        continue;
                    }

                    fs_dent->fsi->mode &= ~TSK_FS_INODE_MODE_FMT;
                    fs_dent->fsi->mode |= TSK_FS_INODE_MODE_REG;
                    fs_dent->ent_type = TSK_FS_DENT_TYPE_REG;
                }
            
                listdent_add_dent(fs_dent, fs_data, dentlist);

                fs_dent->fsi->mode = mode;
                fs_dent->ent_type = ent_type;
            } else if (fs_data->type == NTFS_ATYPE_IDXROOT) {
                printed = 1;

                /* If it is . or .. only print it if the flags say so,
                 * we continue with other streams though in case the 
                 * directory has a data stream 
                 */
                if (!(TSK_FS_ISDOT(fs_dent->name))) 
                    listdent_add_dent(fs_dent, fs_data, dentlist);
            }

            fs_data = fs_data->next;
        }

	    /* A user reported that an allocated file had the standard
	     * attributes, but no $Data.  We should print something */
	    if (printed == 0) {
            listdent_add_dent(fs_dent, fs_data, dentlist);
	    }

    } else {
        /* skip it if it is . or .. and we don't want them */
        if (!(TSK_FS_ISDOT(fs_dent->name)))
            listdent_add_dent(fs_dent, NULL, dentlist);
    }
    return TSK_WALK_CONT;
}

/* callback function for dent_walk used by listdir */
static TSK_WALK_RET_ENUM listdent_walk_callback_list(TSK_FS_INFO *fs, TSK_FS_DENT *fs_dent, void *ptr) {
    PyObject *tmp;
    PyObject *list = (PyObject *)ptr;

    /* we dont want to add '.' and '..' */
    if(TSK_FS_ISDOT(fs_dent->name))
        return TSK_WALK_CONT;

    tmp = PyString_FromString(fs_dent->name);
    PyList_Append(list, tmp);
    Py_DECREF(tmp);

    return TSK_WALK_CONT;
}

/* callback function for file_walk, populates a block list */
static TSK_WALK_RET_ENUM
getblocks_walk_callback(TSK_FS_INFO *fs, TSK_DADDR_T addr, char *buf, size_t size, TSK_FS_BLOCK_FLAG_ENUM flags, void *ptr) {

    struct block *b;
    skfile *file = (skfile *) ptr;

    if(size <= 0)
        return TSK_WALK_CONT;

    if(!(flags & TSK_FS_BLOCK_FLAG_RES)) {
        /* create a new block entry */
        b = talloc(file->blocks, struct block);
        b->addr = addr;
        b->size = size;
        list_add_tail(&b->list, &file->blocks->list);
    }

    file->size += size;

    /* add to the list */
    return TSK_WALK_CONT;
}

/* lookup an inode from a path */
TSK_INUM_T lookup_inode(TSK_FS_INFO *fs, char *path) {
    TSK_INUM_T ret;
    char *tmp = talloc_strdup(NULL,path);
    /* this is evil and modifies the path! */
    tsk_fs_ifind_path(fs, 0, tmp, &ret);
    talloc_free(tmp);
    return ret;
}

/* callback for lookup_path */
//static uint8_t
TSK_WALK_RET_ENUM
lookup_path_cb(TSK_FS_INFO * fs, TSK_FS_DENT * fs_dent, void *ptr) {
    struct dentwalk *dent = (struct dentwalk *)ptr;

    if (fs_dent->inode == dent->inode) {
        dent->path = talloc_asprintf(dent, "/%s%s", fs_dent->path, fs_dent->name);
        return TSK_WALK_STOP;
    }
    return TSK_WALK_CONT;
}

/* lookup path for inode, supply an dentwalk ptr (must be a talloc context),
 * name will be filled in */
int lookup_path(TSK_FS_INFO *fs, struct dentwalk *dent) {
    int flags = TSK_FS_DENT_FLAG_RECURSE | TSK_FS_DENT_FLAG_ALLOC;

    /* special case, the walk won't pick this up */
    if(dent->inode == fs->root_inum) {
        dent->path = talloc_strdup(dent, "/");
        return 0;
    }

    /* there is a walk optimised for NTFS */
    if((fs->ftype & TSK_FS_INFO_TYPE_FS_MASK) == TSK_FS_INFO_TYPE_NTFS_TYPE) {
        if(ntfs_find_file(fs, dent->inode, 0, 0, flags, lookup_path_cb, (void *)dent))
            return 1;
    } else {
        if(fs->dent_walk(fs, fs->root_inum, flags, lookup_path_cb, (void *)dent))
            return 1;
    }
    return 0;
}

/* parse an inode string into inode, type, id */
int parse_inode_str(char *str, TSK_INUM_T *inode, uint32_t *type, uint32_t *id) {
    char *ptr;

    errno = 0;
    *inode = strtoull(str, &ptr, 10);
    if(errno != 0)
        return 0;

    if(*ptr == '-')
        *type = strtoul(ptr+1, &ptr, 10);
    if(*ptr == '-')
        *id = strtoul(ptr+1, &ptr, 10);

    return 1;
}

void print_current_exception() {
  PyErr_WriteUnraisable(PyErr_Occurred());
};

/* The methods below implement an sleuthkit img interface backed by any python
 * file-like object using the python abstract object layer, though specific 
 * optimisations might be made if the object based on a pyflag "iosubsys", or
 * other C-code backed interface.
 */

/* Return the size read and -1 if error */
static ssize_t
pyfile_read_random(TSK_IMG_INFO * img_info, TSK_OFF_T vol_offset, char *buf,
                   size_t len, TSK_OFF_T offset) {

    PyObject *res;
#if PY_VERSION_HEX >= 0x02050000 // python2.5
    Py_ssize_t read;
#else
    int read;
#endif
    unsigned  long long int tot_offset;
    char *strbuf;
    IMG_PYFILE_INFO *pyfile_info = (IMG_PYFILE_INFO *) img_info;
    tot_offset = offset + vol_offset;

    /* seek to correct offset */
    res = PyObject_CallMethod(pyfile_info->fileobj, "seek", "(K)", tot_offset);
    if(res == NULL) {
      print_current_exception();
        tsk_errno = TSK_ERR_IMG_SEEK;
        snprintf(tsk_errstr, TSK_ERRSTR_L, "pyfile_read_random - can't seek to %llu", tot_offset);
        tsk_errstr2[0] = '\0';
        return -1;
    } else
      Py_DECREF(res);
	
    /* try the read */
    res = PyObject_CallMethod(pyfile_info->fileobj, "read", "(k)", len);
    if(res == NULL) {
      print_current_exception();
        tsk_errno = TSK_ERR_IMG_SEEK;
        snprintf(tsk_errstr, TSK_ERRSTR_L, "pyfile_read_random - can't read %llu from %llu", (uint64_t)len, (uint64_t)tot_offset);
        tsk_errstr2[0] = '\0';
        return -1;
    }

    /* retrieve data */
    if(PyString_AsStringAndSize(res, &strbuf, &read) == -1) {
        tsk_errno = TSK_ERR_IMG_SEEK;
        snprintf(tsk_errstr, TSK_ERRSTR_L, "pyfile_read_random - error retrieving data");
        tsk_errstr2[0] = '\0';
        Py_DECREF(res);
        return -1;
    }
    // Make sure we dont overflow our buffer
    if(read > len) read = len;

    memcpy(buf, strbuf, read);

    Py_DECREF(res);

    return read;
}

TSK_OFF_T
pyfile_get_size(TSK_IMG_INFO * img_info) {
    return img_info->size;
}

void
pyfile_imgstat(TSK_IMG_INFO * img_info, FILE * hFile) {
    fprintf(hFile, "IMAGE FILE INFORMATION\n");
    fprintf(hFile, "--------------------------------------------\n");
    fprintf(hFile, "Image Type: pyflag iosubsys\n");
    fprintf(hFile, "\nSize in bytes: %" PRIuOFF "\n", img_info->size);
    return;
}

void
pyfile_close(TSK_IMG_INFO * img_info) {
  if(img_info) {
    IMG_PYFILE_INFO *pyfile_info = (IMG_PYFILE_INFO *)img_info;

    Py_DECREF(pyfile_info->fileobj);
    talloc_free(img_info);
  };
  return;
}

/* construct an IMG_PYFILE_INFO */
TSK_IMG_INFO *
pyfile_open(PyObject *fileobj) {
    IMG_PYFILE_INFO *pyfile_info;
    TSK_IMG_INFO *img_info;
    PyObject *tmp, *tmp2;

    //if ((pyfile_info = (IMG_PYFILE_INFO *) mymalloc(sizeof(IMG_PYFILE_INFO))) == NULL)
    pyfile_info = talloc(NULL, IMG_PYFILE_INFO);
    if(pyfile_info == NULL)
        return NULL;

    memset((void *) pyfile_info, 0, sizeof(IMG_PYFILE_INFO));

    /* do some checks on the object 
    if((PyObject_CallMethod(fileobj, "read", "(i)", 0) == NULL) || 
       (PyObject_CallMethod(fileobj, "seek", "(i)", 0) == NULL)) {
        return NULL;
    }
    */

    /* store the object */
    pyfile_info->fileobj = fileobj;
    Py_INCREF(fileobj);

    /* setup the TSK_IMG_INFO struct */
    img_info = (TSK_IMG_INFO *) pyfile_info;

    img_info->itype = TSK_IMG_INFO_TYPE_PYFILE_TYPE;
    img_info->read_random = pyfile_read_random;
    img_info->get_size = pyfile_get_size;
    img_info->close = pyfile_close;
    img_info->imgstat = pyfile_imgstat;

    img_info->size = 0;

    /** FIXME: Sometimes the file like object has an attribute .size
	which should avoid us doing this 
    */

    /* this block looks aweful! */
    tmp = PyObject_CallMethod(fileobj, "seek", "(ii)", 0, 2);
    if(tmp) {
        Py_DECREF(tmp);
        tmp = PyObject_CallMethod(fileobj, "tell", NULL);
        if(tmp) {
            tmp2 = PyNumber_Long(tmp);
            if(tmp2) {
	      img_info->size = PyLong_AsUnsignedLongLong(tmp2);
	      Py_DECREF(tmp2);
            }
	    Py_DECREF(tmp);
        }
        tmp = PyObject_CallMethod(fileobj, "seek", "(i)", 0);
	if(!tmp) return NULL;
	Py_DECREF(tmp);
    }

    return img_info;
}


/*****************************************************************
 * Now for the python module stuff 
 * ***************************************************************/

/************* SKFS ***************/
static PyObject *skfs_close(skfs *self) {
    if(self->fs)
        self->fs->close(self->fs);
    if(self->img)
        self->img->close(self->img);

    Py_RETURN_NONE;
}

static void
skfs_dealloc(skfs *self) {
  skfs_object_count --;

  // This is not deeded here as skfs_close gets called already
  //  Py_DECREF(skfs_close(self));

  if(self->root_inum) {
    Py_DECREF(self->root_inum);
  };

  talloc_free(self->context);
  self->ob_type->tp_free((PyObject*)self);
}

static int
skfs_init(skfs *self, PyObject *args, PyObject *kwds) {
    PyObject *imgfile;
    char *fstype=NULL;
    long long int imgoff=0;
    self->root_inum = NULL;

    static char *kwlist[] = {"imgfile", "imgoff", "fstype", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "O|Ks", kwlist, 
				    &imgfile, &imgoff, &fstype))
        return -1; 

    /** Create a NULL talloc context for us. Now everything will be
	allocated against that.
    */
    self->context = talloc_size(NULL,1);

    /* initialise the img and filesystem */
    tsk_error_reset();

    self->img = pyfile_open(imgfile);
    if(!self->img) {
      char *error = error_get();

      PyErr_Format(PyExc_IOError, "Unable to open image file: %s", error);
      return -1;
    }

    /* initialise the filesystem */
    tsk_error_reset();
    self->fs = tsk_fs_open(self->img, imgoff, fstype);
    if(!self->fs) {
      char *error = error_get();
      PyErr_Format(PyExc_RuntimeError, "Unable to open filesystem in image: %s", error);
      return -1;
    }

    /* build and store the root inum */
    self->root_inum = (PyObject *)PyObject_New(skfs_inode, &skfs_inodeType);
    ((skfs_inode *)self->root_inum)->inode = self->fs->root_inum;
    ((skfs_inode *)self->root_inum)->type = 0;
    ((skfs_inode *)self->root_inum)->id = 0;
    ((skfs_inode *)self->root_inum)->alloc = 1;

    self->block_size = self->fs->block_size;
    self->first_block = self->fs->first_block;
    self->last_block = self->fs->last_block;

    skfs_object_count++;
    return 0;
}

/* return a list of files and directories */
static PyObject *
skfs_listdir(skfs *self, PyObject *args, PyObject *kwds) {
    PyObject *list;
    char *path=NULL;
    TSK_INUM_T inode;
    int flags=0;
    /* these are the boolean options with defaults */
    int alloc=1, unalloc=0;

    static char *kwlist[] = {"path", "alloc", "unalloc", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "s|ii", kwlist, 
                                     &path, &alloc, &unalloc))
        return NULL; 

    tsk_error_reset();
    inode = lookup_inode(self->fs, path);
    if(inode == 0) {
      char *error = error_get();
      return PyErr_Format(PyExc_IOError, "Unable to find inode for path %s: %s", path, error);
    };

    /* set flags */
    if(alloc)
        flags |= TSK_FS_DENT_FLAG_ALLOC;
    if(unalloc)
        flags |= TSK_FS_DENT_FLAG_UNALLOC;

    list = PyList_New(0);

    tsk_error_reset();
    self->fs->dent_walk(self->fs, inode, flags, listdent_walk_callback_list, (void *)list);
    if(tsk_errno) {
      char *error = error_get();
      Py_DECREF(list);
      return PyErr_Format(PyExc_IOError, "Unable to list inode %llu: %s", inode, error);
    };

    return list;
}

/* Open a file from the skfs */
static PyObject *
skfs_open(skfs *self, PyObject *args, PyObject *kwds) {
    char *path=NULL;
    PyObject *inode=NULL;
    int ret;
    PyObject *fileargs, *filekwds;
    skfile *file;

    static char *kwlist[] = {"path", "inode", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|sO", kwlist, &path, &inode))
        return NULL; 

    /* make sure we at least have a path or inode */
    if(path==NULL && inode==NULL)
        return PyErr_Format(PyExc_SyntaxError, "One of path or inode must be specified");

    /* create an skfile object and return it */
    fileargs = PyTuple_New(0);
    if(path && inode) {
      filekwds = Py_BuildValue("{sOsssO}", "filesystem", (PyObject *)self, 
			       "path", path, "inode", inode);
    } else if(inode) {
      filekwds = Py_BuildValue("{sOsO}", "filesystem", (PyObject *)self, 
                                 "inode", inode);
    } else {
      filekwds = Py_BuildValue("{sOss}", "filesystem", (PyObject *)self, 
                                 "path", path);
    };

    if(!filekwds) return NULL;

    file = PyObject_New(skfile, &skfileType);
    file->context = talloc_size(NULL,1);
    ret = skfile_init(file, fileargs, filekwds);
    Py_DECREF(fileargs);
    Py_DECREF(filekwds);

    if(ret == -1) {
        Py_DECREF(file);
        return NULL;
    }
    return (PyObject *)file;
}

/* perform a filesystem walk (like os.walk) */
static PyObject *
skfs_walk(skfs *self, PyObject *args, PyObject *kwds) {
    char *path=NULL;
    int alloc=1, unalloc=0, ret;
    int names=1, inodes=0;
    PyObject *fileargs, *filekwds;
    skfs_walkiter *iter;
    TSK_INUM_T inode=0;

    static char *kwlist[] = {"path", "inode", "alloc", "unalloc", "names", "inodes", NULL};


    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|sKiiii", kwlist, &path, &inode, 
                                    &alloc, &unalloc, &names, &inodes))
        return NULL; 

    /* create an skfs_walkiter object to return to the caller */
    fileargs = PyTuple_New(0);
    if(path)
        filekwds = Py_BuildValue("{sOsssKsisisisi}", "filesystem", (PyObject *)self, "path", path,
                                 "inode", inode, "alloc", alloc, "unalloc", unalloc, "names", names, "inodes", inodes);
    else
        filekwds = Py_BuildValue("{sOsKsisisisi}", "filesystem", (PyObject *)self, 
                                 "inode", inode, "alloc", alloc, "unalloc", unalloc, "names", names, "inodes", inodes);

    iter = PyObject_New(skfs_walkiter, &skfs_walkiterType);

    ret = skfs_walkiter_init(iter, fileargs, filekwds);
    Py_DECREF(fileargs);
    Py_DECREF(filekwds);

    if(ret == -1) {
        Py_XDECREF(iter);
        return NULL;
    }
    return (PyObject *)iter;
}

/* perform an inode walk, return a list of inodes. This is best only used to
 * find unallocated (deleted) inodes as it builds a list in memory and returns
 * it (skfs.walk by contrast uses a generator). */
static PyObject *
skfs_iwalk(skfs *self, PyObject *args, PyObject *kwds) {
    int alloc=0, unalloc=1;
    int flags=TSK_FS_INODE_FLAG_UNALLOC | TSK_FS_INODE_FLAG_USED;
    PyObject *list;
    TSK_INUM_T inode=0;

    static char *kwlist[] = {"inode", "alloc", "unalloc", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|Kii", kwlist, &inode, 
                                    &alloc, &unalloc))
        return NULL; 

    // ignore args for now and just do full walk (start->end)
    list = PyList_New(0);
    self->fs->inode_walk(self->fs, self->fs->first_inum, self->fs->last_inum, flags, 
            inode_walk_callback, (void *)list);

    return list;
}

PyObject *build_stat_result(TSK_FS_INODE *fs_inode) {
  PyObject *result,*os;
  
  /* return a real stat_result! */
  /* (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime) */
  os = PyImport_ImportModule("os");
  // I cant imagine it ever failing but...
  if(!os) return NULL;

  result = PyObject_CallMethod(os, "stat_result", "((iKiiiiKlll))", 
			       (int)fs_inode->mode, (unsigned PY_LONG_LONG)fs_inode->addr, 
			       (int)0, (int)fs_inode->nlink, 
			       (int)fs_inode->uid, (int)fs_inode->gid, 
			       (unsigned PY_LONG_LONG)fs_inode->size,
			       (long int)fs_inode->atime, (long int)fs_inode->mtime, 
			       (long int)fs_inode->ctime);
  Py_DECREF(os);
  
  return result;
};

/* stat a file */
static PyObject *
skfs_stat(skfs *self, PyObject *args, PyObject *kwds) {
    PyObject *result;
    PyObject *inode_obj;
    char *path=NULL;
    TSK_INUM_T inode=0;
    TSK_FS_INODE *fs_inode;
    int type=0, id=0;

    static char *kwlist[] = {"path", "inode", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|sO", kwlist, &path, &inode_obj))
        return NULL; 

    /* make sure we at least have a path or inode */
    if(path==NULL && inode_obj==NULL)
        return PyErr_Format(PyExc_SyntaxError, "One of path or inode must be specified");

    if(path) {
        tsk_error_reset();
        inode = lookup_inode(self->fs, path);
        if(inode == 0) {
	  char *error = error_get();
	  return PyErr_Format(PyExc_IOError, "Unable to find inode for path %s: %llu: %s", path, inode,  error);
	};
    } else {
        /* inode can be an int or a string */
        if(PyNumber_Check(inode_obj)) {
            PyObject *l = PyNumber_Long(inode_obj);
            inode = PyLong_AsUnsignedLongLong(l);
            Py_DECREF(l);
        } else {
            if(!parse_inode_str(PyString_AsString(inode_obj), &inode, &type, &id))
                return PyErr_Format(PyExc_IOError, "Inode must be a long or a string of the format \"inode[-type-id]\"");
        }
    }

    /* can we lookup this inode? */
    tsk_error_reset();
    fs_inode = self->fs->inode_lookup(self->fs, inode);
    if(fs_inode == NULL) {
      char *error = error_get();
      return PyErr_Format(PyExc_IOError, "Unable to find inode %llu: %s", inode, error);
    };

    result = build_stat_result(fs_inode);

    /* release the fs_inode */
    tsk_fs_inode_free(fs_inode);
    return result;
}

/* stat an already open skfile */
static PyObject *
skfs_fstat(skfs *self, PyObject *args) {
    PyObject *result, *skfile_obj;
    TSK_FS_INODE *fs_inode;

    if(!PyArg_ParseTuple(args, "O", &skfile_obj))
        return NULL; 

    /* check the type of the file object */
    if(PyObject_TypeCheck(skfile_obj, &skfileType) == 0) {
        PyErr_Format(PyExc_TypeError, "file is not an skfile instance");
        return NULL;
    }

    fs_inode = ((skfile *)skfile_obj)->fs_inode;
    
    //FIXME should we set size to be skfile->size? It is not the same as
    //fs_inode->size when we have the non-default attribute open.
    result = build_stat_result(fs_inode);
    return result;
}

/* read link */
static PyObject *
skfs_readlink(skfs *self, PyObject *args, PyObject *kwds) {
    PyObject *result;
    PyObject *inode_obj;
    char *path=NULL;
    TSK_INUM_T inode=0;
    TSK_FS_INODE *fs_inode;
    int type=0, id=0;

    static char *kwlist[] = {"path", "inode", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|sO", kwlist, &path, &inode_obj))
        return NULL; 

    /* make sure we at least have a path or inode */
    if(path==NULL && inode_obj==NULL)
        return PyErr_Format(PyExc_SyntaxError, "One of path or inode must be specified");

    if(path) {
        tsk_error_reset();
        inode = lookup_inode(self->fs, path);
        if(inode == 0) {
	  char *error = error_get();
	  return PyErr_Format(PyExc_IOError, "Unable to find inode for path %s: %llu: %s", path, inode,  error);
	};
    } else {
        /* inode can be an int or a string */
        if(PyNumber_Check(inode_obj)) {
            PyObject *l = PyNumber_Long(inode_obj);
            inode = PyLong_AsUnsignedLongLong(l);
            Py_DECREF(l);
        } else {
            if(!parse_inode_str(PyString_AsString(inode_obj), &inode, &type, &id))
                return PyErr_Format(PyExc_IOError, "Inode must be a long or a string of the format \"inode[-type-id]\"");
        }
    }

    /* can we lookup this inode? */
    tsk_error_reset();
    fs_inode = self->fs->inode_lookup(self->fs, inode);
    if(fs_inode == NULL) {
      char *error = error_get();
      return PyErr_Format(PyExc_IOError, "Unable to find inode %llu: %s", inode, error);
    };

    if(S_ISLNK(fs_inode->mode) && fs_inode->link) {
        result = PyString_FromString(fs_inode->link);
    } else {
    	result = PyErr_Format(PyExc_IOError, "Invalid Arguement, not a link");
    }

    /* release the fs_inode */
    tsk_fs_inode_free(fs_inode);
    return result;
}

/* this new object is requred to support the iterator protocol for skfs.walk
 * */
static void 
skfs_walkiter_dealloc(skfs_walkiter *self) {
    if(self->skfs)
      Py_XDECREF(self->skfs);

    talloc_free(self->context);
    self->ob_type->tp_free((PyObject*)self);
}

static int 
skfs_walkiter_init(skfs_walkiter *self, PyObject *args, PyObject *kwds) {
    char *path=NULL;
    PyObject *skfs_obj;
    struct dentwalk *root;
    int alloc=1, unalloc=0;
    int names=1, inodes=0;
    TSK_INUM_T inode;

    static char *kwlist[] = {"filesystem", "path", "inode", "alloc", "unalloc", "names", "inodes", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "O|sKiiii", kwlist, &skfs_obj, 
                                    &path, &inode, &alloc, &unalloc, &names, &inodes))
        return -1; 

    /* must have at least inode or path */
    if(inode == 0 && path == NULL) {
        PyErr_Format(PyExc_SyntaxError, "One of filename or inode must be specified");
        return -1;
    };

    /* setup the talloc context */
    self->context = talloc_size(NULL, 1);

    /* set flags */
    self->flags = self->myflags = 0;
    if(alloc)
        self->flags |= TSK_FS_DENT_FLAG_ALLOC;
    if(unalloc)
        self->flags |= TSK_FS_DENT_FLAG_UNALLOC;
    if(names)
        self->myflags |= SK_FLAG_NAMES;
    if(inodes)
        self->myflags |= SK_FLAG_INODES;

    /* incref the skfs */
    Py_INCREF(skfs_obj);
    self->skfs = (skfs *)skfs_obj;

    /* Initialise the path stack */
    self->walklist = talloc(self->context, struct dentwalk);
    INIT_LIST_HEAD(&self->walklist->list);

    /* add the start path */
    root = talloc(self->walklist, struct dentwalk);
    root->type = root->id = 0;
    root->alloc = 1;

    if(inode == 0) {
        tsk_error_reset();
        root->inode = lookup_inode(self->skfs->fs, path);
    } else root->inode = inode;

    if(path == NULL) {
        tsk_error_reset();
        lookup_path(self->skfs->fs, root);
    } else root->path = talloc_strdup(root, path);

    list_add(&root->list, &self->walklist->list);

    return 0;
}

static PyObject *skfs_walkiter_iternext(skfs_walkiter *self) {
    PyObject *dirlist, *filelist, *root, *result, *inode;
    struct dentwalk *dw, *dwlist;
    struct dentwalk *dwtmp, *dwtmp2;
    char *tmp;

    /* are we done ? */
    if(list_empty(&self->walklist->list))
        return NULL;

    /* pop an item from the stack */
    list_next(dw, &self->walklist->list, list);

    /* initialise our list for this walk */
    dwlist = talloc(self->walklist, struct dentwalk);
    INIT_LIST_HEAD(&dwlist->list);

    /* walk this directory */
    tsk_error_reset();
    self->skfs->fs->dent_walk(self->skfs->fs, dw->inode, self->flags, 
                              listdent_walk_callback_dent, (void *)dwlist);
    /* must ignore errors and keep going or else we kill the whole walk because one dirlist failed! */
    if(tsk_errno) {
        tsk_error_reset();
        //PyErr_Format(PyExc_IOError, "Walk error at (%d)%s: %s", dw->inode, dw->path, tsk_error_get());
        //talloc_free(dwlist);
        //return NULL;
    }

    /* process the list */
    dirlist = PyList_New(0);
    filelist = PyList_New(0);
    list_for_each_entry_safe(dwtmp, dwtmp2, &dwlist->list, list) {

        PyObject *inode_val, *name_val, *inode_name_val;
        
        /* build all the objects */
        inode_val = (PyObject *)PyObject_New(skfs_inode, &skfs_inodeType);
        ((skfs_inode *)inode_val)->inode = dwtmp->inode;
        ((skfs_inode *)inode_val)->type = dwtmp->type;
        ((skfs_inode *)inode_val)->id = dwtmp->id;
        ((skfs_inode *)inode_val)->alloc = dwtmp->alloc;

        name_val = PyString_FromString(dwtmp->path);
        inode_name_val = Py_BuildValue("(OO)", inode_val, name_val);

        /* process directories */
        if(dwtmp->ent_type & TSK_FS_DENT_TYPE_DIR) {

            /* place into dirlist */
            if((self->myflags & SK_FLAG_INODES) && (self->myflags & SK_FLAG_NAMES))
                PyList_Append(dirlist, inode_name_val);
            else if(self->myflags & SK_FLAG_INODES)
                PyList_Append(dirlist, inode_val);
            else if(self->myflags & SK_FLAG_NAMES)
                PyList_Append(dirlist, name_val);

            /* steal it and push onto the directory stack */
            if(dwtmp->alloc == 1) {
                talloc_steal(self->walklist, dwtmp);
                tmp = dwtmp->path;
                if(strcmp(dw->path, "/") == 0)
                    dwtmp->path = talloc_asprintf(dwtmp, "/%s", tmp);
                else
                    dwtmp->path = talloc_asprintf(dwtmp, "%s/%s", dw->path, tmp);
                talloc_free(tmp);
                list_move(&dwtmp->list, &self->walklist->list);
            }

        } else {
            /* place into filelist */
             if((self->myflags & SK_FLAG_INODES) && (self->myflags & SK_FLAG_NAMES))
                PyList_Append(filelist, inode_name_val);
            else if(self->myflags & SK_FLAG_INODES)
                PyList_Append(filelist, inode_val);
            else if(self->myflags & SK_FLAG_NAMES)
                PyList_Append(filelist, name_val);
        }

        Py_DECREF(inode_name_val);
        Py_DECREF(name_val);
        Py_DECREF(inode_val);
    }

    /* now build root */
    inode = (PyObject *)PyObject_New(skfs_inode, &skfs_inodeType);
    ((skfs_inode *)inode)->inode = dw->inode;
    ((skfs_inode *)inode)->type = dw->type;
    ((skfs_inode *)inode)->id = dw->id;
    ((skfs_inode *)inode)->alloc = dw->alloc; //(dw->flags & TSK_FS_DENT_FLAG_ALLOC) ? 1 : 0;

    if((self->myflags & SK_FLAG_INODES) && (self->myflags & SK_FLAG_NAMES))
        root = Py_BuildValue("(Ns)", inode, dw->path);
    else if(self->myflags & SK_FLAG_INODES)
        root = inode;
    else if(self->myflags & SK_FLAG_NAMES)
        root = PyString_FromString(dw->path);
    else {
        Py_DECREF(inode);
        Py_INCREF(Py_None);
        root = Py_None;
    }

    result = Py_BuildValue("(NNN)", root, dirlist, filelist);

    /* now delete this entry from the stack */
    list_del(&dw->list);
    talloc_free(dw);
    talloc_free(dwlist);

    return result;
}

/************** SKTSK_FS_INODE **********/
static int skfs_inode_init(skfs_inode *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"inode", "type", "id", "alloc", NULL};
    int alloc;

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "Kiii", kwlist, 
                                    &self->inode, &self->type, &self->id, &alloc))
        return -1; 
    self->alloc = alloc ? 1 : 0;
    return 0;
}

static PyObject *skfs_inode_str(skfs_inode *self) {
    PyObject *result;
    char *str;
    str = talloc_asprintf(NULL, "%llu-%u-%u", self->inode, self->type, self->id);
    if(self->alloc == 2)
      str = talloc_asprintf_append(str, "*");

    result = PyString_FromString(str);
    talloc_free(str);
    return result;
}

static PyObject *
skfs_inode_getinode(skfs_inode *self, void *closure) {
    return PyLong_FromUnsignedLongLong(self->inode);
}

static PyObject *
skfs_inode_getalloc(skfs_inode *self, void *closure) {
    return PyInt_FromLong((long)self->alloc);
}

static PyObject *skfs_inode_long(skfs_inode *self) {
    return PyLong_FromUnsignedLongLong(self->inode);
}

/************* SKFILE ***************/
static void
skfile_dealloc(skfile *self) {
    Py_XDECREF(self->skfs);
    talloc_free(self->context);
    /* Not really needed now 
    if(self->blocks)
      talloc_free(self->blocks);
    */
    if(self->fs_inode)
      tsk_fs_inode_free(self->fs_inode);
    skfile_object_count--;
    self->ob_type->tp_free((PyObject*)self);
}

static int
skfile_init(skfile *self, PyObject *args, PyObject *kwds) {
    char *filename=NULL;
    PyObject *inode_obj=NULL;
    TSK_INUM_T inode=0;
    PyObject *skfs_obj;
    TSK_FS_INFO *fs;
    int flags;

    self->type = 0;
    self->id = 0;
    self->skfs = NULL;

    static char *kwlist[] = {"filesystem", "path", "inode", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "O|sO", kwlist, 
                                    &skfs_obj, &filename, &inode_obj))
        return -1; 

    /* check the type of the filesystem object */
    if(PyObject_TypeCheck(skfs_obj, &skfsType) == 0) {
        PyErr_Format(PyExc_TypeError, "filesystem is not an skfs instance");
        return -1;
    }

    fs = ((skfs *)skfs_obj)->fs;

    /* must specify either inode or filename */
    if(filename==NULL && inode_obj==NULL) {
        PyErr_Format(PyExc_SyntaxError, "One of filename or inode must be specified");
        return -1;
    };

    if(filename) {
        tsk_error_reset();
        inode = lookup_inode(fs, filename);
        if(inode == 0) {
	  char *error = error_get();
	  PyErr_Format(PyExc_IOError, "Unable to find inode for file %s: %s", filename, error);
	  return -1;
        }
    } else {
        /* inode can be an int or a string */
        if(PyNumber_Check(inode_obj)) {
            PyObject *l = PyNumber_Long(inode_obj);
            inode = PyLong_AsUnsignedLongLong(l);
            Py_DECREF(l);
        } else {
            if(!parse_inode_str(PyString_AsString(inode_obj), &inode, &self->type, &self->id)) {
                PyErr_Format(PyExc_IOError, "Inode must be a long or a string of the format \"inode[-type-id]\"");
                return -1;
            }
        }
    }

    /* can we lookup this inode? */
    tsk_error_reset();
    self->fs_inode = fs->inode_lookup(fs, inode);
    if(self->fs_inode == NULL) {
      char *error = error_get();
      PyErr_Format(PyExc_IOError, "Unable to find inode: %s", error);
      return -1;
    };

    /* store a ref to the skfs */
    Py_INCREF(skfs_obj);
    self->skfs = skfs_obj;

    self->readptr = 0;
    self->size = 0;

    /* perform a file run and populate the block list, use type and id to
     * ensure we follow the correct attribute for NTFS (these default to 0
     * which will give us the default data attribute). size will also be set
     * during the walk */

    flags = TSK_FS_FILE_FLAG_AONLY | TSK_FS_FILE_FLAG_RECOVER | TSK_FS_FILE_FLAG_NOSPARSE;
    if(self->id == 0)
        flags |= TSK_FS_FILE_FLAG_NOID;

    self->blocks = talloc(self->context, struct block);
    INIT_LIST_HEAD(&self->blocks->list);
    tsk_error_reset();
    fs->file_walk(fs, self->fs_inode, self->type, self->id, flags,
                 getblocks_walk_callback, (void *)self);
    if(tsk_errno) {
      char *error = error_get();
      PyErr_Format(PyExc_IOError, "Error reading inode: %s", error);
      return -1;
    };

    skfile_object_count++;
    return 0;
}

static PyObject *
skfile_str(skfile *self) {
    PyObject *result;
    char *str;
    str = talloc_asprintf(self->context, "%llu-%u-%u", self->fs_inode->addr, self->type, self->id);
    result = PyString_FromString(str);
    talloc_free(str);
    return result;
}

static PyObject *
skfile_read(skfile *self, PyObject *args, PyObject *kwds) {
    char *buf;
    Py_ssize_t written;
    PyObject *retdata;
    TSK_FS_INFO *fs;
    Py_ssize_t readlen=-1;
    int slack=0, overread=0;
    off_t maxsize;

    fs = ((skfs *)self->skfs)->fs;

    static char *kwlist[] = {"size", "slack", "overread", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|iii", kwlist, 
                                    &readlen, &slack, &overread))
        return NULL; 

    /* return slack? It is hard to calculate slack length correctly. You cannot
     * simply set it to blocksize * numblocks. Consider a compressed NTFS file.
     * It may have 2 blocks allocated, but filesize > 2 * blocksize before
     * slack is considered at all. In this case we can't really return slack at
     * all so dont bother. */
    if(slack)
        maxsize = max(self->size, list_count(&self->blocks->list) * fs->block_size);
    else
        maxsize = self->size;

    /* overread file? If set, the read operation will read past the end of the
     * file into the next block on the filesystem. Overread can be set to True
     * (represented in python as the integer 1) in which case it will overread
     * by 256 bytes, alternatively if overread is set to any other possitive
     * value, overread will read up to that value past the end of the file.
     * This option allows searching tools to look for signatures which occur in
     * an overlap between file slack and the next filesystem block */
    if(slack && overread) {
        if(overread == 1) overread = 256;
        maxsize += overread;
    }

    /* adjust readlen if size not given or is too big */
    if(readlen < 0 || self->readptr + readlen > maxsize)
        readlen = maxsize - self->readptr;

    if(readlen < 0) readlen = 0;

    // Allocate the output string as a python String so there is no
    // copying:
    retdata = PyString_FromStringAndSize(NULL, readlen);
    if(readlen==0) return retdata;

    if(!retdata)
      return NULL;
    
    buf = PyString_AsString(retdata);

    if(self->type == 0 && self->id == 0)
        if(slack)
            written = tsk_fs_read_file_noid_slack(fs, self->fs_inode, self->readptr, readlen, buf);
        else
            written = tsk_fs_read_file_noid(fs, self->fs_inode, self->readptr, readlen, buf);
    else
        if(slack)
            written = tsk_fs_read_file_slack(fs, self->fs_inode, self->type, self->id, self->readptr, readlen, buf);
        else
            written = tsk_fs_read_file(fs, self->fs_inode, self->type, self->id, self->readptr, readlen, buf);

    /* perform overread if necessary have to use direct block IO for this */
    if(slack && overread && written < readlen) {
        TSK_DADDR_T last_block = 0;
        struct block *b;
        TSK_DATA_BUF *blockbuf;
        int len;
	int to_read = readlen - written;
	TSK_DADDR_T start;
	int count = 0;

	if(to_read > overread) to_read = overread;

        list_for_each_entry(b, &self->blocks->list, list) {
            last_block = b->addr;
	    count ++;
        }

        /* increment to get the next block after the file */
        last_block++;

	start = last_block * fs->block_size;

	// If the readptr actually points past the end of the file, we
	// may need to start reading a bit ahead.
	if(self->readptr > count * fs->block_size)
	  start += (self->readptr - count * fs->block_size);
        /* do a direct block read from the filesystem */
        len = tsk_fs_read_random(fs, buf+written, to_read, start);
        if(len > 0) {
	        written += len;
        }
    }

    /*
    if(readlen != written) {
        talloc_free(buf);
        return PyErr_Format(PyExc_IOError, "Failed to read all data: wanted %d, got %d", readlen, written);
	}
    */

    // Make sure we dont return any extra data
    _PyString_Resize(&retdata, written);

    self->readptr += readlen;
    return retdata;
}

static PyObject *
skfile_seek(skfile *self, PyObject *args, PyObject *kwds) {
    int64_t offset=0;
    int whence=0;
    int slack=0, overread=0;
    off_t maxsize;
    TSK_FS_INFO *fs;

    fs = ((skfs *)self->skfs)->fs;

    static char *kwlist[] = {"offset", "whence", "slack", "overread", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kwds, "L|iii", kwlist, 
                                    &offset, &whence, &slack, &overread))
        return NULL; 

    /* return slack? It is hard to calculate slack length correctly. You cannot
     * simply set it to blocksize * numblocks. Consider a compressed NTFS file.
     * It may have 2 blocks allocated, but filesize > 2 * blocksize before
     * slack is considered at all. In this case we can't really return slack at
     * all so dont bother. */
    if(slack)
        maxsize = max(self->size, list_count(&self->blocks->list) * fs->block_size);
    else
        maxsize = self->size;

    /* overread file? If set, the read operation will read past the end of the
     * file into the next block on the filesystem. Overread can be set to True
     * (represented in python as the integer 1) in which case it will overread
     * by 256 bytes, alternatively if overread is set to any other possitive
     * value, overread will read up to that value past the end of the file.
     * This option allows searching tools to look for signatures which occur in
     * an overlap between file slack and the next filesystem block */
    if(slack && overread) {
        if(overread == 1) overread = 256;
        maxsize += overread;
    }

    switch(whence) {
        case 0:
            self->readptr = offset;
            break;
        case 1:
            self->readptr += offset;
            break;
        case 2:
            self->readptr = maxsize + offset;
            break;
        default:
            return PyErr_Format(PyExc_IOError, "Invalid argument (whence): %d", whence);
    }

    Py_RETURN_NONE;
}

static PyObject *
skfile_tell(skfile *self) {
    return PyLong_FromLongLong(self->readptr);
}

static PyObject *
skfile_blocks(skfile *self) {
    struct block *b;
    PyObject *tmp;

    PyObject *list = PyList_New(0);
    list_for_each_entry(b, &self->blocks->list, list) {
        tmp = PyLong_FromUnsignedLongLong(b->addr);
        PyList_Append(list, tmp);
        Py_DECREF(tmp);
    }
    return list;
}

static PyObject *
skfile_close(skfile *self) {
  Py_RETURN_NONE;
}

/* This is a thin wrapper around mmls. It returns a tuple of four-tuples
 * containing (start, len, desc, type) for each partition. Note that type is
 * NOT partition type but is actually defined as:
 #define MM_TYPE_DESC    0x01
 #define MM_TYPE_VOL     0x02
*/

static TSK_WALK_RET_ENUM
mmls_callback(TSK_MM_INFO *mminfo, TSK_PNUM_T num, TSK_MM_PART *part, int flag, void *ptr) {
    PyObject *partlist = (PyObject *)ptr;
    PyObject *thispart;

    thispart = Py_BuildValue("(KKsi)", part->start, part->len, part->desc, part->type);
    PyList_Append(partlist, thispart);
    Py_DECREF(thispart);
    return TSK_WALK_CONT;
}

static PyObject *mmls(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *imgfile;
    PyObject *partlist;
    char *mmtype=NULL;
    int imgoff=0;
    TSK_IMG_INFO *img;
    TSK_MM_INFO *mm;

    static char *kwlist[] = {"imgfile", "imgoff", "mmtype", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "O|is", kwlist, 
				    &imgfile, &imgoff, &mmtype))
        return NULL; 

    /** Create a NULL talloc context for us. Now everything will be
	allocated against that.  */
    /* initialise the img */
    tsk_error_reset();
    img = pyfile_open(imgfile);
    if(!img) {
      char *error = error_get();
      PyErr_Format(PyExc_IOError, "Unable to open image file: %s", error);
      return NULL;
    }

    /* initialise the mm layer */
    tsk_error_reset();
    mm = tsk_mm_open(img, imgoff, mmtype);
    if(!mm) {
      char *error = error_get();
      PyErr_Format(PyExc_IOError, "Unable to read partition table: %s", error);
      img->close(img);
      return NULL;
    }

    /* walk primary partition table */
    partlist = PyList_New(0);
    tsk_error_reset();
    if(mm->part_walk(mm, mm->first_part, mm->last_part, 0, mmls_callback, (void *)partlist)) {
      char *error = error_get();

        PyErr_Format(PyExc_IOError, "Partition walk failed: %s", error);
        Py_DECREF(partlist);
        mm->close(mm);
        img->close(img);
        return NULL;
    }

    /* now, what to do about extended partitions??? */

    /* cleanup */
    mm->close(mm);
    img->close(img);

    return partlist;
}

/* these are the module methods */
static PyMethodDef sk_methods[] = {
    {"mmls", (PyCFunction)mmls, METH_VARARGS|METH_KEYWORDS,
     "Read Partition TablRead Partition Table" },
    {NULL, NULL, 0, NULL}  /* Sentinel */
};

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
initsk(void) 
{
    PyObject* m;

    //talloc_enable_leak_report_full();

    /* create module */
    m = Py_InitModule3("sk", sk_methods,
                       "Sleuthkit module.");

    /* setup skfs type */
    skfsType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&skfsType) < 0)
        return;

    Py_INCREF(&skfsType);
    PyModule_AddObject(m, "skfs", (PyObject *)&skfsType);

    //    talloc_enable_leak_report();

    /* setup skfs_walkiter type */
    skfs_walkiterType.tp_new = PyType_GenericNew;
    skfs_walkiterType.tp_iter = PyObject_SelfIter;

    if (PyType_Ready(&skfs_walkiterType) < 0)
        return;

    Py_INCREF(&skfs_walkiterType);

    /* setup inode type */
    skfs_inodeType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&skfs_inodeType) < 0)
        return;

    Py_INCREF(&skfs_inodeType);
    PyModule_AddObject(m, "skinode", (PyObject *)&skfs_inodeType);

    /* setup skfile type */
    skfileType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&skfileType) < 0)
        return;

    Py_INCREF(&skfileType);
    PyModule_AddObject(m, "skfile", (PyObject *)&skfileType);
}
