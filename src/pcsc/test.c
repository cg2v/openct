#include <stdio.h>

#ifdef __APPLE__
#include <PCSC/wintypes.h>
#include <PCSC/pcsclite.h>
#include <PCSC/ifdhandler.h>
#else
#include <wintypes.h>
#include <pcsclite.h>
#include <ifdhandler.h>
#endif

int main(int argc, char **argv) {

  RESPONSECODE r;
  unsigned char ATR[65];
  unsigned char CMD[6] = {0x0, 0xCA, 0x80, 0x00, 0x40};
  unsigned char RSP[65];
  SCARD_IO_HEADER h,rh;
  size_t s;
  if (argc < 2)
    return -1;
  r = IFDHCreateChannelByName(0, argv[1]);
  printf("create return is %d\n", r);

  if (r == IFD_SUCCESS) {
    r = IFDHICCPresence(0);
    printf("presence return is %d\n", r);

    if (r == IFD_SUCCESS) {
      s = sizeof(ATR);
      r = IFDHPowerICC(0, IFD_POWER_UP, ATR, &s);
      printf("powerup return is %d\n", r);
      if (r == IFD_SUCCESS) {
	int i;
	printf("ATR is ");
	for (i=0; i < s; i++)
	  printf("%s%x", i > 0 ? ":" : "", ATR[i]);
	printf("\n");
	h.Protocol=1;
	s = sizeof(RSP);
	r = IFDHTransmitToICC(0, h, CMD, 5, RSP, &s, &rh);
	printf("transmit return is %d\n", r);
	if (r == IFD_SUCCESS) {
	  int i;
	  printf("Response is ");
	  for (i=0; i < s; i++)
	    printf("%s%x", i > 0 ? ":" : "", RSP[i]);
	  printf("\n");
	}
	r = IFDHPowerICC(0, IFD_POWER_DOWN, NULL, NULL);
	printf("powerdown return is %d\n", r);
      }
    }
    r = IFDHCloseChannel(0);
    printf("close return is %d", r);
  }
  return 0;
}
