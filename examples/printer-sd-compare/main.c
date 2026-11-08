/* RAM-only SD photo comparison: one PRINT press prints 0:/PRINT.JPG in Gray mode and then,
 * if the first print completed and the decoded frame is unchanged, in Dot mode.
 * Both use 100% energy and the same geometry as printer-sd-demo. */
#define APX_PRINTER_SD_BOTH 1
#include "../printer-sd-demo/main.c"
