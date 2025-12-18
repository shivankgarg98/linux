#ifndef LINUX_SDXI_ADMIN_H
#define LINUX_SDXI_ADMIN_H

struct sdxi_cxt;

int sdxi_adm_start_cxt(struct sdxi_cxt *cxt);
void sdxi_adm_stop_cxt(struct sdxi_cxt *cxt);

#endif /* LINUX_SDXI_ADMIN_H  */
