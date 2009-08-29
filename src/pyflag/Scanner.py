#!/usr/bin/env python
# ******************************************************
# Copyright 2004: Commonwealth of Australia.
#
# Developed by the Computer Network Vulnerability Team,
# Information Security Group.
# Department of Defence.
#
# Michael Cohen <scudette@users.sourceforge.net>
# David Collett <daveco@users.sourceforge.net>
#
# ******************************************************
#  Version: FLAG  $Version: 0.87-pre1 Date: Thu Jun 12 00:48:38 EST 2008$
# ******************************************************
#
# * This program is free software; you can redistribute it and/or
# * modify it under the terms of the GNU General Public License
# * as published by the Free Software Foundation; either version 2
# * of the License, or (at your option) any later version.
# *
# * This program is distributed in the hope that it will be useful,
# * but WITHOUT ANY WARRANTY; without even the implied warranty of
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# * GNU General Public License for more details.
# *
# * You should have received a copy of the GNU General Public License
# * along with this program; if not, write to the Free Software
# * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
# ******************************************************
""" This module implements a scanning mechanism for operating on all files within a given filesyst

Scanners are pieces of code that are run on all the files in a filesystem when the filesystem is loaded. The purpose of scanners is to extract meta data about files in the filesystem and make deductions.

The GenScan abstract class documents a Generic scanner. This scanner is applied on every file in a filesystem during a run of the FileSystem's scan method.

Scanners are actually factory classes and must be inherited from GenScanFactory. 
"""
import pyflag.conf
config=pyflag.conf.ConfObject()
import pyflag.pyflaglog as pyflaglog
import os,imp, StringIO
import re,pdb
import pyflag.Registry as Registry
import pyflag.DB as DB
import pyflag.FlagFramework as FlagFramework
import fnmatch
import ScannerUtils
import pyflag.CacheManager as CacheManager
import pyflag.Magic as Magic

class BaseScanner:
    """ This is the actual scanner class that will be instanitated once for each file in the filesystem.
    
    factories is a list of factory scanner objects that should be used to scan new files that have been revealed due to this particular scanner. This is mostly used for iteratively scanning files found inside other files (e.g. zip archieves etc). If this scanner is not adding new files to the ddfs tables, you do not need to use this.
    outer is a reference to the generator object that is used to instantiate these classes.
    
    Note that this is a nested class since it may only be instantiated by first instantiating a Factory object. """
    def __init__(self, inode_id,ddfs,outer,factories=None,fd=None):
        """
        @arg fd: The file descriptor which is being scanned. Note that scanners must not interfere with fd (i.e. never change the current file pointer).
        """
        self.inode_id = inode_id
        self.fd=fd
        self.size = 0
        self.ddfs = ddfs
        self.case=outer.case
        self.outer=outer
        self.factories=factories
        
        # This flag indicates if we wish to be ignored from now on -
        # it may be set if we determining that the file is boring, and
        # do not wish to get any more of it. The default ignore state
        # is taken from the fd itself - sometimes fds wish to be
        # ignored and only selected by specific scanners. Other times
        # the fds want to be processed by anyone.
        try:
            self.ignore = self.fd.ignore
        except:
            self.ignore = False

    def process(self, data):
        """ process the chunk of data.

        This function is given a chunk of data from the file - this
        may not be the complete file. Sometimes it may be
        appropropriate to accumulate the data until the finish method
        is called (See the StoreAndScan classes for examples).
        
        @arg data: Some limited amount of data from the file. The size of the data is unspecified but more than 1000 bytes.        
        """
        pass

    def slack(self, data):
        """ process file slack space.

        This function is called with file slack data once all regular file
        data has been processed (by the 'process' method) and before finish is
        called. By default, this method is a noop, many scanners will not use
        it (e.g. MD5 scanner does not care about slack). Those that do want to
        see slack (e.g. the index scanner) can simply provide this method.
        UPDATE: The slack method is also passed a small amount of data (200
        bytes) from the next contiguous block in the filesystem in order to
        detect signatures which cross the boundary from slack into unallocated
        space. This can happen (for example) when a file is deleted and a new
        (smaller) one allocated in its place.
        """
        pass

    def finish(self):
        """ all data has been provided to process, finish up.

        Note that this signals the completion of the file. If a file had been queued during the process call, it should now be processed.
        """
        pass

class GenScanFactory:
    """ Abstract Base class for scanner Factories.
    
    The Scanner Factory is a specialised class for producing scanner
    objects. It will be instantiated once at the begining of the run,
    and destroyed at the end of the run. It will be expected to
    produce a new Scanner object for each file in the filesystem.
    """
    ## Should this scanner be on by default?
    default=False
    
    ## The default group - this will cause all scanners to go to this
    ## scanner group by default.
    group = 'GeneralForensics'

    ## This is a list of scanner names which we depend on. Depending
    ## on a scanner will force it to be enabled whenever we are
    ## enabled.
    depends = []

    ## This is the name of the group which this scanner will render
    ## under in the GUI:
    group = ''
    
    def __init__(self,fsfd=None):
        """ Factory constructor.

        @arg fsfd: A filesystem object for the filesystem we are about to scan.
        """
        self.fsfd = fsfd
        if fsfd:
            self.case = fsfd.case

    def prepare(self):
        """ This is called before the scanner is used.

        Generally the constructor should be very brief (because it might be called to reset rather than to actually scan). And most work should be done in this method.
        """

    def destroy(self):
        """ Final destructor called on the factory to finish the scan operation.

        This is sometimes used to make indexes etc. 
        """
        pass

    def reset(self, inode_id):
        """ This method drops the relevant tables in the database, restoring the db to the correct state for rescanning to take place. """
        pyflaglog.log(pyflaglog.WARNING, "The reset function is now deprecated. All calls should be to multiple_inode_reset which allows more efficient resets and also allows you to specify a single inode anyway")

    def multiple_inode_reset(self, inode_glob):
        """ This method modifies the database to reset the scanners. It takes an argument which is a glob of the inodes to be reset. It does this for performance reasons. Each scanner is expected to clean up after itself. """

        ## Here we do the default (clear scanner_cache field) and hope that inherited classes either deal with it or call us
        sql = fnmatch.translate(inode_glob)
        db = DB.DBO(self.case)
        db.execute("update inode set scanner_cache = REPLACE(scanner_cache, %r, '') where inode rlike %r", (self.__class__.__name__, sql))
                   

    def reset_entire_path(self, path_glob):
        """ This method modifies the database to reset the scanners. It takes an argument which is a path under which all inodes will be reset. It does this for performance reasons. Each scanner is expected to clean up after itself. """

        ## The scanners should do their thing on their tables and then call this (the base class) method to allow us to handle the simple stuff (clear the scanner cache field. If they don't call us, it is up to them to clean it up themselves.
        path = path_glob
        if not path.endswith("*"): path = path + "*"  
        db = DB.DBO(self.case)
        db.execute("update vfs set scanner_cache = REPLACE(scanner_cache, %r, '') where path rlike %r",(self.__class__.__name__, fnmatch.translate(path)))
        
    ## Relative order of scanners - Higher numbers come later in the order
    order=10

    def scan(self, fd, factories, type, mime):
        """ This is the new scan method - scanners just implement this
        method and go from there.
        """

    ## Deprecated
    class Scan(BaseScanner):
        """ The Scan class must be defined as an inner class to the factory. """

StoreAndScanFiles = set()

class MemoryScan(BaseScanner):
    """ A scanner designed to scan buffers of text in memory.

    This scanner implements a sliding window, i.e. each buffer scanned begins with OVERLAP/BUFFERSIZE from the previous buffer. This allows regex, and virus definitions to locate matches that are broken across a block boundary.
    """
    windowsize=200
    def __init__(self, inode_id,ddfs,outer,factories=None,fd=None):
        BaseScanner.__init__(self, inode_id,ddfs,outer,factories,fd=fd)
        self.window = ''
        self.offset=0

    def process(self, data):
        buf = self.window + data
        self.process_buffer(buf)
        self.offset += len(buf)
        self.window = buf[-self.windowsize:]
        self.offset -= len(self.window)

    def process_buffer(self,buf):
        """ This abstract method should implement the actual scanner.

        @arg offset: The actual offset the buffer begins with inside the inode.
        """

class StoreAndScan(BaseScanner):
    """ A Scanner designed to store a temporary copy of the scanned
    file to be able to invoke an external program on it.

    Note that this is a scanner inner class (which should be defined
    inside the factory class). This class should be extended by
    Scanner factories to provide real implementations to the 'boring'
    and 'external_process' methods.
    """
    def __init__(self, inode_id,ddfs,outer,factories=None,fd=None):
        BaseScanner.__init__(self, inode_id,ddfs,outer,factories, fd=fd)
        self.file = None
        self.boring_status = True

    def boring(self, data=''):
        """ This function decides if this file is boring (i.e. we should ignore it).

        This must be implemented in derivative classes.
        
        @return: True if the file is boring (i..e should be ignored), False if we are interested in it.
        """

    def process(self, data):
        try:
            ## If this file is boring, we check to see if there is new
            ## information which makes it not boring:
            if self.boring_status:
                self.boring_status = self.boring(data=data)
                
            ## We store all the files we create in a central place, so
            ## multiple instances of StoreAndScan can all share the
            ## same physical file (as long as they all want to give it
            ## the same name). This allows more efficient streamlining
            ## as all StoreAndScan derivatives can use the same file,
            ## but only one is actually responsible for creating it.

            ## Whats happening here is that a number of StoreAndScan
            ## scanners may be called in turn. The first one will open
            ## the file and add to the StoreAndScanFiles set. All
            ## subsequent scanners will ignore the file. The first
            ## scanner is responsible for writing the file into the
            ## cache file.
            if not self.file and not self.boring_status and \
                   self.inode_id not in StoreAndScanFiles:
                self.file = CacheManager.MANAGER.create_cache_fd(self.case,
                                                                 self.inode_id)
                StoreAndScanFiles.add(self.inode_id)
        except KeyError:
            pass

        if self.file:
            self.file.write(data)

    def finish(self):
        if not self.boring_status:
            try:
                self.file.flush()
                self.file.close()
            except: pass
            
            ## We reuse our own fd for this - each external process
            ## gets the same fd but we rewind it first. External
            ## processes can act on the fd directly or call
            ## CacheManager.MANAGER.provide_cache_filename() to get a
            ## cached filename.
            fd = self.fd
            fd.seek(0)
            self.external_process(fd)

        if self.file:
            self.file.close()
            ## We now remove the file from the central storage place:
            StoreAndScanFiles.remove(self.inode_id)

    def external_process(self,fd):
        """ This function is invoked by the scanner to process a temporary file.

        This function my be overridden by derived classes.

        @arg name: The name of the file in the filesystem to operate on - The Scanner should have saved this file previously.
        """
        
class StoreAndScanType(StoreAndScan):
    """ This class scans a file only if a file is of a certain type.

    The determination of the type of a file is done by using the metadata passed from the type scanner, or failing that we query the type table for the given inode.
    """
    ## These are the mime types that will be used to decide if we should scan this file
    types = []
    
    def boring(self, data=''):
        import pyflag.Magic as Magic

        m = Magic.MagicResolver()
        self.type, self.mime_type = m.find_inode_magic(self.case, inode_id=self.inode_id,
                                                       data=data)
        for t in self.types:
            if re.search(t,self.mime_type) or re.search(t, self.type):
                return False
            
        self.ignore = True
        return True

class StringIOType(StoreAndScanType):
    """ Just like StoreAndScanType but the file exists in memory only.
    """
    def process(self, data):
        try:
            if self.boring_status:
                self.boring_status = self.boring(data=data)

            if not self.file and not self.boring_status and\
                   self.inode_id not in StoreAndScanFiles:
                StoreAndScanFiles.add(self.inode_id)
                self.file = StringIO.StringIO()
        except KeyError:
            pass

        if self.file:
            self.file.write(data)

    def finish(self):
        if self.file:
            StoreAndScanFiles.remove(self.inode_id)

        if not self.boring_status:
            self.file.seek(0)
            self.external_process(self.file)

class ScanIfType(StoreAndScanType):
    """ Only Scans if the type matches self.types.

    Just like StoreAndScanType but without creating the file.
    """
    def __init__(self, inode_id,ddfs,outer,factories=None,fd=None):
        BaseScanner.__init__(self, inode_id,ddfs,outer,factories,fd=fd)
        self.boring_status = True

    def process(self, data):
        ## If this file is boring, we check to see if there is new
        ## information which makes it not boring:
        if self.boring_status:
            self.boring_status = self.boring(data=data)

    def finish(self):
        print "Scanned type %s" % self.fd.inode_id
        pass

def resetfile(ddfs, inode_id,factories):
    for f in factories:
        dbh=DB.DBO(ddfs.case)
        f.reset(inode_id)
        dbh.execute("update vfs set scanner_cache = REPLACE(scanner_cache,%r,'') where inode_id=%r",
                                (f.__class__.__name__, inode_id))

MESSAGE_COUNT = 0

### This is used to scan a file with all the requested scanner factories
def scan_inode(case, inode_id, factories):
    """ Scans the given inode_id with all the factories provided. Each
    factory is used to instantiate a Scan() object, then we call
    Scan.scan() on the inode_id.
    """
    import pyflag.FileSystem as FileSystem

    fsfd = FileSystem.DBFS(case)
    fd = fsfd.open(inode_id=inode_id)
    stat = fd.stat()
    
    # instantiate a scanner object from each of the factory. We only
    # instantiate scanners from factories which have not been run on
    # this inode previously. We find which factories were already run
    # by checking the inode table.  Note that we still pass the full
    # list of factories to the Scan class so that it may invoke all of
    # the scanners on new files it discovers.
    dbh = DB.DBO(case)    
    dbh.execute("select inode_id, scanner_cache from vfs where inode_id=%r limit 1",
                fd.inode_id)
    row=dbh.fetch()
    try:
        scanners_run =row['scanner_cache'].split(',')
    except:
        scanners_run = []

    fd.inode_id = row['inode_id']

    ## The new scanning framework is much simpler - we just call the
    ## scan() method on each factory.
    m = Magic.MagicResolver()
    messages = DB.expand("Scanning file %s/%s (inode %s)",
                         (stat['path'],stat['name'],stat['inode_id']))

    for c in factories:
        if c.__class__.__name__ not in scanners_run:
            type, mime = m.find_inode_magic(case, fd.inode_id)
            fd.seek(0)
            try:
                c.scan(fd, factories=factories, type=type, mime=mime)
            except:
                pdb.post_mortem()
    
    global MESSAGE_COUNT
    MESSAGE_COUNT += 1
    if not MESSAGE_COUNT % 50:
        pyflaglog.log(pyflaglog.DEBUG, messages)
    else:
        pyflaglog.log(pyflaglog.VERBOSE_DEBUG, messages)

#     while 1:
#         try:
#             data = fd.read(buffsize)
#             if not data: break
#         except IOError,e:
#             break

#         # call process method of each class
#         interest = 0

#         for o in objs:
#             if not o.ignore:
#                 interest+=1

#         ## If none of the scanners are interested with this file, we
#         ## stop right here
#         if not interest:
#             pyflaglog.log(pyflaglog.VERBOSE_DEBUG, "No interest for %s", fd.inode_id)
#             break
        
#         for o in objs:
#             try:
#                 if not o.ignore:
#                     interest+=1
#                     pyflaglog.log(pyflaglog.VERBOSE_DEBUG, "Processing with %s", o)
#                     o.process(data)

#             except Exception,e:
#                 pyflaglog.log(pyflaglog.ERRORS,"Scanner (%s) Error: %s" %(o,e))
#                 # Ignore the error and keep going here:
#                 #raise

#         if not interest:
#             pyflaglog.log(pyflaglog.DEBUG, "No interest for %s", fd.inode_id)
#             break

#     # call slack method of each object. fd.slack must be reset after the call
#     # because the scanners actually have a copy of fd and some of them actually
#     # use it and get confused if it returns slack. Also pass overread.
#     fd.slack=True
#     fd.overread=True
#     data = fd.read()
#     fd.slack=False
#     fd.overread=False
#     if data:
#         for o in objs:
#             try:
#                 o.slack(data)
#             except Exception,e:
#                 pyflaglog.log(pyflaglog.ERRORS,"Scanner (%s) Error: %s" %(o,e))

#     # call finish method of each object
#     for o in objs:
#         try:
#             o.finish()
#         except Exception,e:
#             pyflaglog.log(pyflaglog.ERRORS,"Scanner (%s) on Inode %s Error: %s" %(o,fd.inode_id,e))

    # Store the fact that we finished in the inode table:
    scanner_names = ','.join([ c.__class__.__name__ for c in factories ])
    try:
        dbh.execute("update vfs set scanner_cache = concat_ws(',',scanner_cache, %r) where inode_id=%r", (scanner_names, fd.inode_id))
    except DB.DBError:
        pass

class Drawer:
    """ This class is responsible for rendering scanners of similar classes.

    This class should be declared as an inner class of the scanner.
    """
    description = "Description of main scanner"
    group = "Name of group"
    child_scanners = None
    default = True

    def __init__(self):
        ## Populate the classes of scanners which depend on us:
        self.child_scanners = []
        for c in Registry.SCANNERS.classes:
            if c.group == self.group:
                ## Its our scanner
                self.child_scanners.append(c)

    def get_group_name(self):
        return "scangroup_%s" % self.group

    def get_parameters(self):
        for i in self.child_scanners:
            try:
                yield "scan_%s" % i.__name__,'onoff'
            except:
                continue

    def add_defaults(self,dest_query,src_query):
        """ Given a src_query object with some scan_ entries, we add
        scan_ entries initialised to their default values until
        the full contained set is represented.
        """
        try:
            scan_group_name = self.get_group_name()

            if src_query[scan_group_name]=='on':
                for i in self.child_scanners:
                    scan_name = 'scan_%s' % i.__name__
                    del dest_query[scan_name]

                    ## If i is not specified, we use the default for
                    ## this scanner:
                    if not src_query.has_key('scan_%s' % i.__name__):
                        if cls.default:
                            dest_query[scan_name]='on'
                        else:
                            dest_query[scan_name]='off'
                    else:
                        dest_query[scan_name]=src_query[scan_name]
        except KeyError:
            pass

    def form(self,query,result):
        left = result.__class__(result)
        scan_group_name = self.get_group_name()

        ## If there is no scan_group defined, we use the default value
        if not query.has_key(scan_group_name):
            if self.default:
                query[scan_group_name]='on'
                result.defaults[scan_group_name]='on'
            else:
                query[scan_group_name]='off'
                result.defaults[scan_group_name]='off'

        ## Add defaults for the scanners contained:
        for cls in self.child_scanners:
            if not query.has_key('scan_%s' % cls.__name__):
                if cls.default:
                    result.hidden('scan_%s' % cls.__name__,'on')
                else:
                    result.hidden('scan_%s' % cls.__name__,'off')
            
        def configure_cb(query,result):
            try:
                if query['refresh']:
                    del query['refresh']

                    result.refresh(0,query,pane="parent")
            except KeyError:
                pass

            ## Draw the gui for all the classes we manage:
            result.decoration = 'naked'
            result.start_form(query,pane="parent")
            result.start_table()

            self.add_defaults(query,query.clone())

            for cls in self.child_scanners:
                scanner_desc = cls.__doc__.splitlines()[0]
                
                ## Add an enable/disable selector
                result.const_selector(scanner_desc,"scan_%s" % cls.__name__,[
                    'on','off'],['Enabled','Disabled'] )
                
            result.end_table()
            result.end_form()

        right=result.__class__(result)
        right.popup(configure_cb,"Configure %s" % self.group,icon="spanner.png")
        left.row(right,self.description)
        result.const_selector(left,
                           scan_group_name,
                           ['on','off'],['Enabled','Disabled'])

## This is a global store for factories:
import pyflag.Store as Store

factories = Store.Store()

def get_factories(case,scanners):
    """ Scanner factories are obtained from the Store or created as
    required. Scanners is a list in the form case:scanner
    """
    ## Ensure dependencies are satisfied
    scanners = ScannerUtils.fill_in_dependancies(scanners)
    
    ## First prepare the required factories:
    result = []
    for scanner in scanners:
        key = DB.expand("%s:%s", (case,scanner))
        try:
            f=factories.get(key)
        except KeyError:
            try:
                cls=Registry.SCANNERS.dispatch(scanner)
            except:
                #pyflaglog.log(pyflaglog.WARNING, "Unable to find scanner for %s", scanner)
                continue

            #Instatiate it:
            import pyflag.FileSystem as FileSystem

            f=cls(FileSystem.DBFS(case))

            ## Initialise it:
            f.prepare()

            ## Store it:
            factories.put(f,key=key)

        result.append(f)

    return result

## These are carvers:
class Carver:
    """ A carver is a class which knows about how to extract specific
    file types.
    """
    regexs = []
    length = 600000
    extension = ''
    
    def __init__(self, fsfd):
        self.fsfd = fsfd

    def get_length(self, fd,offset):
        """ Returns the length of the carved image from the inode
        fd. fd should already be seeked to the right place.
        """
        length = min(fd.size-offset, self.length)
        return length
    
    def add_inode(self, fd, offset, factories):
        """ This is called to allow the Carver to add VFS inodes.

        Returns the prospective length of the file. This does not have
        to be the same as the length returned by get_length() - it
        just directs the CarveScan to ignore matches which fall within
        this range within our parent.
        """
        ## Calculate the length of the new file
        length = self.get_length(fd,offset)
        new_inode = "%s|o%s:%s" % (fd.inode_id, offset, length)

        self._add_inode(new_inode, length, "%s.%s" % (offset, self.extension),
                        fd, factories)
        return length

    def _add_inode(self, new_inode, length, name, fd, factories):
        pathname = self.fsfd.lookup(inode_id = fd.inode_id)
        ## By default we just add a VFS Inode for it.
        self.fsfd.VFSCreate(None,
                            new_inode,
                            pathname + "/" +name,
                            size = length,
                            )

        ## Dont scan the remaining inodes because this may cause many false positives
        ## Scan the new inodes:
        #new_fd = self.fsfd.open(inode = new_inode)

        ## dont carve the resultant file (or we could get recursive carves)
        #factories = [ f for f in factories if "Carv" not in f.__class__.__name__]
        #scanfile(self.fsfd, new_fd, factories)
