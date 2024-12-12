#include <libtxproto/fifo_bufferref.h>

#define FRENAME(x)     BUFFERREF_FIFO_ ## x
#define RENAME(x)      sp_bufferref_ ##x
#define PRIV_RENAME(x) bufferref ##x
#define FNAME          enum SPBufferRefFIFOFlags
#define SNAME          SPBufferRefFIFO
#define FREE_FN        av_buffer_unref
#define CLONE_FN(x)    ((x) ? av_buffer_ref((x)) : NULL)
#define TYPE           AVBufferRef

#include "fifo_template.c"

#undef TYPE
#undef CLONE_FN
#undef FREE_FN
#undef SNAME
#undef FNAME
#undef PRIV_RENAME
#undef RENAME
#undef FRENAME
