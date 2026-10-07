#ifndef MISTER_RAST_H
#define MISTER_RAST_H

// The FPGA rasteriser of the Dethrace core (core/rtl/dethrace_rast_top.sv) as
// backend of BRender's pentprim driver (drivers/pentprim/fpgarast.h).

// Starts a session with the rasteriser and makes it draw the triangles.
// Returns 0 if the loaded core has none.
int MiSTer_Rast_Open(void);
void MiSTer_Rast_Close(void);

#endif
