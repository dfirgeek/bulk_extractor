/*
 * bulk_extractor.cpp:
 * Feature Extraction Framework...
 *
 */

#include "bulk_extractor.h"
#include "bulk_extractor_api.h"
#include "image_process.h"
#include "threadpool.h"
#include "be13_api/aftimer.h"
#include "histogram.h"
#include "dfxml/src/dfxml_writer.h"
#include "dfxml/src/hash_t.h"

#include "phase1.h"

#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <set>
#include <setjmp.h>
#include <vector>
#include <queue>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>

/****************************************************************
 *** 
 *** Here is the bulk_extractor API
 *** It is under development.
 ****************************************************************/

/* The API relies on a special version of the feature recorder set
 * that writes to a callback function instead of a
 */

class callback_feature_recorder;
class callback_feature_recorder_set;

/* a special feature_recorder_set that calls a callback rather than writing to a file.
 * Typically we will instantiate a single object called the 'cfs' for each BEFILE.
 * It creates multiple named callback_feature_recorders, but they all callback through the same
 * callback function using the same set of locks
 */ 


static std::string hash_name("md5");
static std::string hash_func(const uint8_t *buf,size_t bufsize)
{
    if(hash_name=="md5" || hash_name=="MD5"){
        return md5_generator::hash_buf(buf,bufsize).hexdigest();
    }
    if(hash_name=="sha1" || hash_name=="SHA1" || hash_name=="sha-1" || hash_name=="SHA-1"){
        return sha1_generator::hash_buf(buf,bufsize).hexdigest();
    }
    if(hash_name=="sha256" || hash_name=="SHA256" || hash_name=="sha-256" || hash_name=="SHA-256"){
        return sha256_generator::hash_buf(buf,bufsize).hexdigest();
    }
    std::cerr << "Invalid hash name: " << hash_name << "\n";
    std::cerr << "This version of bulk_extractor only supports MD5, SHA1, and SHA256\n";
    exit(1);
}
static feature_recorder::hash_def my_hasher(hash_name,hash_func);

class callback_feature_recorder_set: public feature_recorder_set {
    // neither copying nor assignment are implemented
    callback_feature_recorder_set(const callback_feature_recorder_set &cfs);
    callback_feature_recorder_set &operator=(const callback_feature_recorder_set&cfs);
    histogram_defs_t histogram_defs;        
public:
    be_callback_t *cb;
    mutable cppmutex Mcb;               // mutex for the callback

public:
    virtual feature_recorder *create_name_factory(const std::string &outdir_,
                                                  const std::string &input_fname_,
                                                  const std::string &name_);
    callback_feature_recorder_set(be_callback_t *cb_):feature_recorder_set(0,my_hasher),histogram_defs(),cb(cb_),Mcb(){
    }

    virtual void init_cfs(){
        feature_file_names_t feature_file_names;
        be13::plugin::scanners_process_enable_disable_commands();
        be13::plugin::get_scanner_feature_file_names(feature_file_names);
        init(feature_file_names,"<NO-INPUT>","<NO-OUTDIR>"); // creates the feature recorders
        be13::plugin::scanners_init(*this); // must be done after feature recorders are created
    }

    virtual void write(const std::string &feature_recorder_name,const std::string &str){
        cppmutex::lock lock(Mcb);
        (*cb)(BULK_EXTRACTOR_API_FLAG_FEATURE,0,
              feature_recorder_name.c_str(),"",str.c_str(),str.size(),"",0);
    }
    virtual void write0(const std::string &feature_recorder_name,
                        const pos0_t &pos0,const std::string &feature,const std::string &context){
        cppmutex::lock lock(Mcb);
        (*cb)(BULK_EXTRACTOR_API_FLAG_FEATURE,0,
              feature_recorder_name.c_str(),pos0.str().c_str(),feature.c_str(),feature.size(),context.c_str(),context.size());
    }

    /* The callback function that will be used to dump a histogram line.
     * it will in turn call the callback function
     */
    static void histogram_dump_callback(void *user,const feature_recorder &fr,
                                        const std::string &str,const uint64_t &count) {
        callback_feature_recorder_set *cfs = (callback_feature_recorder_set *)(user);
        assert(cfs!=0);
        assert(cfs->cb!=0);
        (*cfs->cb)(BULK_EXTRACTOR_API_FLAG_HISTOGRAM,count, fr.name.c_str(),"",str.c_str(),str.size(),"",0);
    }
};



class callback_feature_recorder: public feature_recorder {
    // neither copying nor assignment are implemented
    callback_feature_recorder(const callback_feature_recorder &cfr);
    callback_feature_recorder &operator=(const callback_feature_recorder&cfr);
    be_callback_t *cb;
public:
    callback_feature_recorder(be_callback_t *cb_,
                              class feature_recorder_set &fs,const std::string &name_):
        feature_recorder(fs,"<no-outdir>","<no-fname>",name_),cb(cb_){
    }
    virtual std::string carve(const sbuf_t &sbuf,size_t pos,size_t len, 
                              const std::string &ext){ // appended to forensic path
        return("");                     // no file created
    }
    virtual void open(){}               // we don't open
    virtual void close(){}               // we don't open
    virtual void flush(){}               // we don't open

    /** write 'feature file' data to the callback */
    virtual void write(const std::string &str){
        dynamic_cast<callback_feature_recorder_set *>(&fs)->write(name,str);
    }
    virtual void write0(const pos0_t &pos0,const std::string &feature,const std::string &context){
        dynamic_cast<callback_feature_recorder_set *>(&fs)->write0(name,pos0,feature,context);
    }
};


/* create_name_factory must be here, after the feature_recorder class is defined. */
feature_recorder *callback_feature_recorder_set::create_name_factory(const std::string &outdir_,
                                                                     const std::string &input_fname_,
                                                                     const std::string &name_){
    //std::cerr << "creating " << name_ << "\n";
    return new callback_feature_recorder(cb,*this,name_);
}



struct BEFILE_t {
    BEFILE_t(be_callback_t cb):fd(),cfs(cb),cfg(){};
    int         fd;
    callback_feature_recorder_set  cfs;
    BulkExtractor_Phase1::Config   cfg;
};

typedef struct BEFILE_t BEFILE;
extern "C" 
BEFILE *bulk_extractor_open(be_callback_t cb)
{
    histogram_defs_t histograms;
    feature_recorder::set_main_threadid();
    scanner_info::scanner_config   s_config; // the bulk extractor config

    s_config.debug       = 0;           // default debug

    be13::plugin::load_scanners(scanners_builtin,s_config);
    //be13::plugin::scanners_process_enable_disable_commands();
    
    //feature_file_names_t feature_file_names;
    //be13::plugin::get_scanner_feature_file_names(feature_file_names);
    
    BEFILE *bef = new BEFILE_t(cb);
    
    /* How do we enable or disable individual scanners? */
    /* How do we set or not set a find pattern? */
    /* We want to disable carving, right? */
    /* How do we create the feature recorder with a callback? */
    return bef;
}
    
extern "C" void bulk_extractor_set_enabled(BEFILE *bef,const char *name,int  mode)
{
    feature_recorder *fr = 0;
    switch(mode){
    case BE_SET_ENABLED_PROCESS_COMMANDS:
        bef->cfs.init_cfs();
        break;

    case BE_SET_ENABLED_SCANNER_DISABLE:
        be13::plugin::scanners_disable(name);
        break;

    case BE_SET_ENABLED_SCANNER_ENABLE:
        be13::plugin::scanners_enable(name);
        break;

    case BE_SET_ENABLED_FEATURE_DISABLE:
        fr = bef->cfs.get_name(name);
        if(fr) fr->set_flag(feature_recorder::FLAG_NO_FEATURES);
        break;

    case BE_SET_ENABLED_FEATURE_ENABLE:
        fr = bef->cfs.get_name(name);
        if(fr) fr->unset_flag(feature_recorder::FLAG_NO_FEATURES);
        break;

    case BE_SET_ENABLED_MEMHIST_ENABLE:
        fr = bef->cfs.get_name(name);
        if(fr) fr->unset_flag(feature_recorder::FLAG_MEM_HISTOGRAM);
        break;

    case BE_SET_ENABLED_DISABLE_ALL:
        be13::plugin::scanners_disable_all();
        break;

    default:
        assert(0);
    }
}



extern "C" 
int bulk_extractor_analyze_buf(BEFILE *bef,uint8_t *buf,size_t buflen)
{
    pos0_t pos0("");
    const sbuf_t sbuf(pos0,buf,buflen,buflen,false);
    be13::plugin::process_sbuf(scanner_params(scanner_params::PHASE_SCAN,sbuf,bef->cfs));
    return 0;
}

extern "C" 
int bulk_extractor_analyze_dev(BEFILE *bef,const char *fname)
{
    struct stat st;
    if(stat(fname,&st)){
        return -1;                  // cannot stat file
    }
    if(S_ISREG(st.st_mode)){
        const sbuf_t *sbuf = sbuf_t::map_file(fname);
        if(!sbuf) return -1;
        be13::plugin::process_sbuf(scanner_params(scanner_params::PHASE_SCAN,*sbuf,bef->cfs));
        delete sbuf;
        return 0;
    }
    return 0;
}

extern "C" 
int bulk_extractor_close(BEFILE *bef)
{
    bef->cfs.dump_histograms((void *)&bef->cfs,
                             callback_feature_recorder_set::histogram_dump_callback,0); //NEED TO SPECIFY THE CALLBACK HERE
    delete bef;
    return 0;
}


    
