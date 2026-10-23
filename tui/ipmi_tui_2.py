#!/usr/bin/env python3
import argparse,asyncio,csv,json,time
from pathlib import Path
from textual.app import App,ComposeResult
from textual.containers import Horizontal,VerticalScroll
from textual.widgets import Footer,Header,Static

SOCKET_PATH="/tmp/ipmi_cluster_monitor.sock"
REFRESH_SECONDS=0.2
SECTIONS={
    "Power": {"units" : {"w","watt","watts"},"names":("power","pwr")},
    "Thermal": {"units" : {"c","°c","celsius","degree c","degrees c","f","°f","fahrenheit","degree f","degrees f","kelvin","k"},"names":("temp","thermal","inlet","exhaust")},
    "Fans": {"units" : {"rpm","revolutions per minute"}, "names" : ("fan",)},
    "Electrical" : {"units" : {"v","volt","volts","mv","millivolt","millivolts","a","amp","amps","ampere","amperes","ma","milliamp","milliamps","%","percent","percentage"}, "names":("volt","current","amp","utilization","usage","percent")},
}
SECTION_ORDER=("Power","Thermal","Fans","Electrical")

def widget_id(host): 
    return "bmc_"+"".join(c if c.isalnum() else "_" for c in host)

def parse_args():
    parser=argparse.ArgumentParser(description="Textual IPMI cluster monitor")
    parser.add_argument("pretty_print",nargs="?",choices=["pretty-print"],help="enable host aliases from pretty_hosts.txt")
    parser.add_argument("pretty_hosts_dir",nargs="?",help="directory containing pretty_hosts.txt (required with pretty-print)")
    args=parser.parse_args()
    
    if args.pretty_print and not args.pretty_hosts_dir: 
        parser.error("pretty-print requires the directory containing pretty_hosts.txt")
    
    if args.pretty_hosts_dir and not args.pretty_print: 
        parser.error("provide pretty_hosts_dir only after pretty-print")
    return args

def load_host_aliases(directory):
    path=Path(directory)/"pretty_hosts.txt"
    aliases={}
    with path.open("r",encoding="utf-8-sig",newline="") as f:
        for row in csv.reader(f,skipinitialspace=True):
            if not row: 
                continue
            
            first=row[0].strip()
            if not first or first.startswith("#"): 
                continue
            if len(row)<2: 
                continue
            host=first
            alias=",".join(row[1:]).strip()
            if host and alias: 
                aliases[host]=alias
    return aliases

def sensor_section(sensor):
    unit=str(sensor.get("unit","")).strip().lower()
    name=str(sensor.get("name","")).strip().lower()
    for section in SECTION_ORDER:
        if unit in SECTIONS[section]["units"]: return section
    if any(x in name for x in ("fan","blower")): 
        return "Fans"
    if any(x in name for x in ("temp","thermal","inlet","exhaust","ambient")): 
        return "Thermal"
    if any(x in name for x in ("power","pwr")): 
        return "Power"
    if any(x in name for x in ("volt","current","amp","utilization","usage","percent")): 
        return "Electrical"
    return None

class BMCBox(Static):
    def __init__(self,host,alias=None):
        super().__init__(id=widget_id(host))
        self.host=host
        self.border_title=alias or host
        self._last_text=None

    def show_data(self,bmc):
        age=max(0.0,time.time()-float(bmc.get("timestamp",0)))
        status="ONLINE" if age<5.0 else f"OFFLINE since {age:.1f}s"
        groups={section:[] for section in SECTION_ORDER}
        for sensor in bmc.get("sensors",[]):
            section=sensor_section(sensor)
            if section: groups[section].append(sensor)
        lines=[f"Status: {status}   Logging: {'ON' if bmc.get('logging') else 'OFF'}",f"Sample: {bmc.get('generation',0)}",""]
        for section in SECTION_ORDER:
            lines.append(f"[b]{section}[/b]")
            sensors=groups[section]
            if not sensors: lines.append("  No matching sensors")
            for s in sensors:
                try: 
                    value=f"{float(s.get('value',0)):>10.3f}"
                except (TypeError,ValueError): 
                    value=f"{str(s.get('value','N/A')):>10}"
                lines.append(f"  {s.get('name','')[:22]:<22} {value} {s.get('unit','')}")
            
            lines.append("")
        
        text="\n".join(lines)
        
        if text!=self._last_text:
            self.update(text)
            self._last_text=text

#CSS="""
#    Screen{layout:vertical}
#    #status{height:1;padding:0 1}
#    #servers{height:1fr;width:100%;padding:1}
#    .bmc-row{width:100%;height:auto;min-height:10}
#    BMCBox{width:1fr;height:auto;min-height:10;margin:0 1;border:round $accent;padding:0 1}
#    """

class IPMI_Cluster_Monitor(App):
    CSS="""
    Screen{layout:vertical}
    #status{height:1;padding:0 1}
    #servers{height:1fr;width:100%;padding:1}
    .bmc-row{width:100%;height:auto;min-height:10}
    BMCBox{width:1fr;height:auto;min-height:10;margin:0 1;border:round $accent;padding:0 1}
    """
    BINDINGS=[("q","quit","Quit"),("r","refresh_now","Refresh")]

    def __init__(self,aliases):
        super().__init__()
        self.aliases=aliases
        self.boxes={}
        self.host_order=[]
        self.refresh_lock=asyncio.Lock()

    def compose(self)->ComposeResult:
        yield Header()
        yield Static("Waiting for collector...",id="status")
        yield VerticalScroll(id="servers")
        yield Footer()

    def on_mount(self):
        self.set_interval(REFRESH_SECONDS,self._scheduled_refresh)
        self.run_worker(self.refresh_data(),exclusive=True)

    def _scheduled_refresh(self):
        if not self.refresh_lock.locked(): self.run_worker(self.refresh_data(),exclusive=True)

    async def request_snapshot(self):
        reader,writer=await asyncio.open_unix_connection(SOCKET_PATH)
        try:
            writer.write(b"GET\\n")
            await writer.drain()
            raw=await reader.read()
            return json.loads(raw.decode())
        finally:
            writer.close()
            await writer.wait_closed()

    async def build_layout(self,bmcs):
        servers=self.query_one("#servers",VerticalScroll)
        old_scroll=servers.scroll_y
        await servers.remove_children()
        self.boxes={}
        self.host_order=[bmc.get("host","unknown") for bmc in bmcs]
        for i in range(0,len(bmcs),2):
            row=Horizontal(classes="bmc-row")
            await servers.mount(row)
            for bmc in bmcs[i:i+2]:
                host=bmc.get("host","unknown")
                box=BMCBox(host,self.aliases.get(host))
                self.boxes[host]=box
                await row.mount(box)
        await asyncio.sleep(0)
        servers.scroll_y=old_scroll

    async def refresh_data(self):
        if self.refresh_lock.locked(): return
        async with self.refresh_lock:
            try:
                data=await self.request_snapshot()
                bmcs=data.get("bmcs",[])
                hosts=[bmc.get("host","unknown") for bmc in bmcs]
                if hosts!=self.host_order: await self.build_layout(bmcs)
                for bmc in bmcs:
                    box=self.boxes.get(bmc.get("host","unknown"))
                    if box is not None: box.show_data(bmc)
                self.query_one("#status",Static).update(f"Collector connected: {len(bmcs)} BMC(s) | Refresh: {REFRESH_SECONDS:.1f}s | Pretty names: {'ON' if self.aliases else 'OFF'}")
            except (FileNotFoundError,ConnectionRefusedError,OSError,json.JSONDecodeError) as e:
                self.query_one("#status",Static).update(f"Collector unavailable: {e}")

    def action_refresh_now(self):
        if not self.refresh_lock.locked(): self.run_worker(self.refresh_data(),exclusive=True)

if __name__=="__main__":
    args=parse_args()
    try: aliases=load_host_aliases(args.pretty_hosts_dir) if args.pretty_print else {}
    except (OSError,csv.Error) as e: raise SystemExit(f"Could not load pretty host aliases: {e}")
    IPMI_Cluster_Monitor(aliases).run()
