## Setup and Package Installations
<pre>
sudo apt update
sudo apt upgrade 
</pre>

### Install FreeIPMI libraries and tools
<pre>
sudo apt install freeipmi libfreeipmi-dev libipmimonitoring-dev build-essential pkg-config
</pre>

## Verify Package Installation and Accessibility
Run these commands to check if the library headers are present in the proper locations and are accessible:
<pre>
dpkg -l | grep -E 'freeipmi|ipmimonitoring'
ls /usr/include/ipmi_monitoring*
</pre>

### Verify if the linker is configured:
<pre>
ldconfig -p | grep ipmimonitoring
</pre>

## Directory Setup
### Create directorie to store executable-
<pre>
mkdir bin
</pre>

## Compiling C Code
### To compile code C project, run from Project Root:
<pre>
gcc -O2 -std=c11 -Wall -Wextra -pthread src/ipmi_ringbuf_clustermon.c -o bin/ipmi_ringbuf_clustermon -lipmimonitoring -lfreeipmi
</pre>
#### The executable will be inside the bin/ directory.

### After compiling with gcc, confirm linkage with:
<pre>
ldd ./sensor_monitor | grep -i ipmi
</pre>
Desirable output: 
<pre>
libipmimonitoring.so.x => /lib/x86_64-linux-gnu/libipmimonitoring.so.x
libfreeipmi.so.y      => /lib/x86_64-linux-gnu/libfreeipmi.so.y
</pre>

## Run
### Note: Please run with sudo privilleges !
<pre>
sudo ./bin/output_executable_name /path/to/bmc/config/file sampling-interval
</pre>

## Starting the Terminal User Interface (TUI) for IPMI Data Visualization
### Open a separate terminal, and run the Python script in tui/ directory- 
<pre>
python3 tui/ipmi_tui_main.py
</pre>
### The TUI shoudl display information cards for each Server BMC specified in the BMC config file and update available sensor data in the window. To display only specfic sensor information, different tabs are provided to switch between sensor types.

### The _ipmi_tui_basic.py_ script, functions the same way, but offers a more minimal interface, without tabs or buttons, simply grouping all sensors based on their type and displaying all of them for each BMC. Run it the same way-
<pre>
python3 tui/ipmi_tui_basic.py
</pre>
