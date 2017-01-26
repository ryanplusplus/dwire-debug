/// DeviceCommand.c



#ifdef windows

  char *DosDevices = 0;
  char *CurrentDevice = 0;

  void LoadDosDevices() {
    if (DosDevices == (char*)-1) {CurrentDevice=0; return;}
    int size = 24576;
    int used = 0;
    while (1) {
      DosDevices = Allocate(size);
      used = QueryDosDevice(0, DosDevices, size);
      if (used) {break;}
      int error = GetLastError();
      Free(DosDevices);
      if (error != ERROR_INSUFFICIENT_BUFFER) {break;}
      size *= 2;
    }
    if (used) {CurrentDevice = DosDevices;}
  }

  void AdvanceCurrentDevice() {
    while (*CurrentDevice) {CurrentDevice++;}
    CurrentDevice++;
    if (!*CurrentDevice) {CurrentDevice = 0;}
  }

  void NextUsbSerialPort() {
    if (!DosDevices) {LoadDosDevices();}
    while (CurrentDevice  &&  strncmp("COM", CurrentDevice, 3)) {AdvanceCurrentDevice();}
    if (CurrentDevice) {
      strncpy(UsbSerialPortName, CurrentDevice, 6);
      UsbSerialPortName[6] = 0;
      AdvanceCurrentDevice();
    } else {
      UsbSerialPortName[0] = 0;
      Free(DosDevices);
      DosDevices = 0;
      CurrentDevice = 0;
    }
  }

#else

  #include <dirent.h>

  DIR *DeviceDir = 0;

  void NextUsbSerialPort() {
    UsbSerialPortName[0] = 0;
    if (!DeviceDir) {DeviceDir = opendir("/dev");
    if (!DeviceDir) {return;}}
    while (!UsbSerialPortName[0]) {
      struct dirent *entry = readdir(DeviceDir);
      if (!entry) {closedir(DeviceDir); DeviceDir=0; return;}
      if (!strncmp("ttyUSB", entry->d_name, 6))
        {strncpy(UsbSerialPortName, entry->d_name, 255); UsbSerialPortName[255] = 0;}
    }
  }

#endif




int ReadByte() {
  u8 byte = 0;
  int bytesRead;

  bytesRead = Read(SerialPort, &byte, 1);
  if (bytesRead == 1) return byte; else return -1;
}


int GetBreakResponseByte(int breaklen) {
  int byte  = 0;

  //Ws(" SerialBreak("); Wd(breaklen,1); Ws(")");
  SerialBreak(breaklen);
  byte = ReadByte();
  if (byte != 0) {Ws(", warning, byte read after break is non-zero.");}

  Ws(", skipping [");
  while (byte == 0)    {Wc('0'); byte = ReadByte();} // Skip zero bytes generated by break.
  while (byte == 0xFF) {Wc('F'); byte = ReadByte();} // Skip 0xFF bytes generated by line going high following break.
  Ws("]");

  //Ws(" GetBreakResponseByte: first "); Wd(first,1); Ws(", byte "); Wd(byte,1);
  return byte;
}



void Wbits(int byte) {
  if (byte < 0) {Wd(byte,1);}
  else for (int i=7; i>=0; i--) {Wc(((byte >> i) & 1) ? '1' : '0');}
}


int approxfactor(int byte) {
  // Determine approximate width of dwire pulse at current baud rate.
  // Returns 100 x pulse width relative to current rate

  int current = (byte >> 7) & 1;  // Start at topmost bit
  int length = 1;
  int longest = 1;
  int avg = -1;
  int count = 0;
  for (int i = 6; i >= 0; i--) {
    int bit = (byte >> i) & 1;
    if (bit == current) {
      length++;
    } else {
      if (longest < length) longest = length;
      if (avg < 0) {
        if (length > 1) {avg = length; count = 1;} else {avg = 0; count = 0;}
      } else {
        avg += length; count ++;
      }
      current = bit;
      length = 1;
    }
  }
  if (longest < length) longest = length;
  if (length > 1) {avg += length; count++;}

  if (count > 0) avg = (100 * avg) / count;
//Ws("longest "); Wd(longest,1); Ws(", avg "); Wd(avg,1);

  int factor;
  if (longest > 2) factor = 100*longest - 70; else factor = avg;
  return factor;
}




int TryBaudRate(int baudrate, int breaklen) {
  // Returns: > 100 - approx factor by which rate is too high (as multiple of 10)
  //          = 100 - rate generates correct 0x55 response
  //          = 0   - port does not support this baud rate

  Ws("Trying "); Ws(UsbSerialPortName); 
  Ws(", baud rate "); Wd(baudrate,5);
  Ws(", breaklen "); Wd(breaklen,4);
  MakeSerialPort(UsbSerialPortName, baudrate);
  if (!SerialPort) {
    Wsl(". Cannot set this baud rate, probably not an FT232."); Wl();
    return 0;
  }

  int byte = GetBreakResponseByte(breaklen);
  CloseSerialPort();
  SerialPort = 0;

  if (byte < 0) {
    Wsl(", No response, giving up."); return 0;
  } else {
    Ws(", received "); Wbits(byte);
  }

  if (byte == 0x55) {
    Wsl(": expected result.");
    return 100;
  }

  return approxfactor(byte);
}




void FindBaudRate() {

  int baudrate = 40000; // Start well above the fastest dwire baud rate based
                        // on the max specified ATtiny clock of 20MHz.
  int breaklen = 50;    // 50ms allows for clocks down to 320KHz.
                        // For 8 MHz break len can be as low as 2ms.

  // First try lower and lower clock rates until we get a good result.
  // The baud rate for each attempt is based on a rough measurement of
  // the relative size of pulses seen in the byte returned after break.

  int factor = TryBaudRate(baudrate, breaklen);

  while (factor > 100) {
    Ws(", factor "); Wd(factor,1); Wsl("%");
    if (factor < 115) factor = 115;
    baudrate = (baudrate * 100) / (factor-10);
    breaklen = 100000 / baudrate; // Now we have the approx byte len we can use a shorter break.
    factor = TryBaudRate(baudrate, breaklen);
  }

  if (factor == 0) return;

  // We have hit a baudrate that returns the right result this one time.
  // Now find a lower and upper bound of working rates in order to
  // finally choose the middle rate.

  breaklen = 100000 / baudrate; // Now we have the approx byte len we can use a shorter break.

  Wsl("Finding upper bound.");

  int upperbound = baudrate;
  do {
    int trial = (upperbound * 102) / 100;
    factor = TryBaudRate(trial, breaklen);
    if (factor == 100) upperbound = trial;
  } while (factor == 100);
  Wl();
  Wsl("Finding lower bound.");

  int lowerbound = baudrate;
  do {
    int trial = (lowerbound * 98) / 100;
    factor = TryBaudRate(trial, breaklen);
    if (factor == 100) lowerbound = trial;
  } while (factor == 100);
  Wl();

  // Finally open the port with the middle of the working range.

  baudrate = (lowerbound + upperbound) / 2;
  Wl(); Ws("Chosen baud rate "); Wd(baudrate,1); Wl(); Wl();
  MakeSerialPort(UsbSerialPortName, baudrate);

  // And check it actually worked.

  int byte = GetBreakResponseByte(breaklen);
  if (byte != 0x55) {CloseSerialPort(); SerialPort = 0;}
}




void TryConnectSerialPort() {
  jmp_buf oldFailPoint;
  memcpy(oldFailPoint, FailPoint, sizeof(FailPoint));

  if (setjmp(FailPoint)) {
    SerialPort = 0;
  } else {
    FindBaudRate();
    if (!SerialPort) {return;}
    DwConnect();
  }

  memcpy(FailPoint, oldFailPoint, sizeof(FailPoint));
}




// Implement ConnectSerialPort called from DwPort.c
void ConnectSerialPort() {
  SerialPort = 0;
  if (UsbSerialPortName[0]) {
    TryConnectSerialPort();
    if (!SerialPort) {Ws("Couldn't connect to DebugWIRE device on "); Fail(UsbSerialPortName);}
  } else {
    while (!SerialPort) {
      NextUsbSerialPort();
      if (!UsbSerialPortName[0]) {Fail("Couldn't find a DebugWIRE device.");}
      TryConnectSerialPort();
    }
  }
}




void DeviceCommand() {
  if (SerialPort) {CloseSerialPort();}
  Sb();
  Ran(ArrayAddressAndLength(UsbSerialPortName));
  ConnectSerialPort();
}
