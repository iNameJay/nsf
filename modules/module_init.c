#include "nsf_event.h"
extern void onRecv(int event,NsfntPkg pkg);
void nsf_module_init()
{
	nsf_register_event(NE_RECV,onRecv);
	nsf_register_event(NE_ACPT,onRecv);
	nsf_register_event(NE_QUIT,onRecv);
}
