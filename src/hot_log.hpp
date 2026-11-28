#pragma once

// Temporary probes (stderr lines "hot ..." and "instrument ...").
// Delete this header and every CMF_HOT_LOG block when the timings are done.
// Turn on with -DCMF_HOT_LOG=1.
#ifndef CMF_HOT_LOG
#define CMF_HOT_LOG 0
#endif
