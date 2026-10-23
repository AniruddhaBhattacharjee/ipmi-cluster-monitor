#!/usr/bin/env python3
import argparse,asyncio,csv,json,time
from pathlib import Path
from textual.app import App,ComposeResult
from textual.containers import Horizontal,VerticalScroll
from textual.widgets import Button,Footer,Header,Static

SOCKET_PATH="/tmp/ipmi_cluster_monitor.sock"
REFRESH_SECONDS=0.5
FILTERS={"all":"All","power":"Power (W)","fan":"Fan (RPM)","temperature":"Temperature","voltage":"Voltage (V)","current":"Current (A)","utilization":"Utilization (%)"}

FILTER_UNITS={
    "power":{"w","watt","watts"},
    "fan":{"rpm","revolutions per minute"},
    "temperature":{"c","°c","celsius","degree c","degrees c","f","°f","fahrenheit","degree f","degrees f"},
    "voltage":{"v","volt","volts","mv","millivolt","millivolts"},
    "current":{"a","amp","amps","ampere","amperes","ma","milliamp","milliamps"},
    "utilization":{"%","percent","percentage"},
}

def widget_id(host):
    return "bmc_"+"".join(c if c.isalnum() else "_" for c in host)

def parse_args():
    parser=argparse.ArgumentParser(description="Textual IPMI cluster monitor")
    parser.add_argument("pretty_print",nargs="?",choices=["pretty-print"],help="enable host aliases from pretty_hosts.txt")
    parser.add_argument("pretty_hosts_dir",nargs="?",help="directory containing pretty_hosts.txt")
    args=parser.parse_args()
    if args.pretty_print and not args.pretty_hosts_dir:
        parser.error("pretty-print requires the directory containing pretty_hosts.txt")
    if args.pretty_hosts_dir and not args.pretty_print:
        parser.error("provide a directory only after pretty-print")
    return args

def load_host_aliases(directory):
    path=Path(directory)/"pretty_hosts.txt"
    aliases={}
    with path.open("r",encoding="utf-8-sig",newline="") as f:
        for row in csv.reader(f,skipinitialspace=True):
            if not row: continue
            host=row[0].strip()
            if not host or host.startswith("#"): continue
            if len(row)<2: continue
            alias=",".join(row[1:]).strip()
            if alias: aliases[host]=alias
    return aliases

def sensor_matches(sensor,category):
    if category=="all": return True
    unit=str(sensor.get("unit","")).strip().lower()
    if unit in FILTER_UNITS.get(category,set()): return True
    name=str(sensor.get("name","")).lower()
    if category=="fan": return "fan" in name and ("rpm" in name or not unit)
    if category=="temperature": return any(x in name for x in ("temp","thermal")) and not unit
    if category=="power": return "power" in name and not unit
    if category=="voltage": return "volt" in name and not unit
    if category=="current": return ("current" in name or " amp" in name) and not unit
    if category=="utilization": return any(x in name for x in ("utilization","usage","percent")) and not unit
    return False

host_dict={
    "172.23.44.141":"DCO-1",
    "172.23.44.142":"DCO-2",
    "172.23.44.143":"DCO-3",
    "172.23.44.144":"DCO-4",
}

class BMCBox(Static):
    def __init__(self,host,display_name=None):
        super().__init__(id=widget_id(host))
        self.host=host
        self.border_title=display_name or host
        self._last_text=None

    def show_data(self,bmc,category):
        age=max(0.0,time.time()-float(bmc.get("timestamp",0)))
        status="ONLINE" if age<5.0 else f"OFFLINE since {age:.1f}s"
        sensors=[s for s in bmc.get("sensors",[]) if sensor_matches(s,category)]
        lines=[f"Status: {status}   Logging: {'ON' if bmc.get('logging') else 'OFF'}",f"Sample: {bmc.get('generation',0)}",""]
        if not sensors: lines.append("No sensors for this category.")
        else:
            for s in sensors:
                try: value=f"{float(s.get('value',0)):>10.3f}"
                except (TypeError,ValueError): value=f"{str(s.get('value','N/A')):>10}"
                lines.append(f"{s.get('name','')[:24]:<24} {value} {s.get('unit','')}")
        text="\n".join(lines)
        if text!=self._last_text:
            self.update(text)
            self._last_text=text

class IPMI_Cluster_Monitor(App):
    CSS="""
    Screen{layout:vertical}
    #status{height:1;padding:0 1}
    #filters{height:3;width:100%;padding:0 1;align-horizontal:left}
    #servers{height:1fr;width:100%;padding:1}
    .bmc-row{width:100%;height:auto;min-height:10}
    BMCBox{width:1fr;height:auto;min-height:10;margin:0 1;border:round $accent;padding:0 1}
    #filters Button{min-width:12;margin:0 1;color:white;text-style:bold}
    #filter_all{background:#596579}
    #filter_power{background:#167d45}
    #filter_fan{background:#8a5a16}
    #filter_temperature{background:#b74736}
    #filter_voltage{background:#2466a8}
    #filter_current{background:#7950a1}
    #filter_utilization{background:#167f86}
    #filters Button.selected{border:heavy white;text-style:bold underline}
    """
    BINDINGS=[("q","quit","Quit"),("r","refresh_now","Refresh")]

    def __init__(self,aliases=None,pretty_enabled=False):
        super().__init__()
        self.aliases=aliases or {}
        self.pretty_enabled=pretty_enabled
        self.boxes={}
        self.host_order=[]
        self.refresh_lock=asyncio.Lock()
        self.category="all"

    def display_name(self,host):
        if self.pretty_enabled: return self.aliases.get(host,host)
        return host

    def compose(self)->ComposeResult:
        yield Header()
        yield Static("Waiting for collector...",id="status")
        with Horizontal(id="filters"):
            for key,label in FILTERS.items():
                yield Button(label,id=f"filter_{key}",classes="selected" if key=="all" else "")
        yield VerticalScroll(id="servers")
        yield Footer()

    def on_mount(self):
        self.set_interval(REFRESH_SECONDS,self._scheduled_refresh)
        self.run_worker(self.refresh_data(),exclusive=True)

    def on_button_pressed(self,event:Button.Pressed):
        button_id=event.button.id or ""
        if not button_id.startswith("filter_"): return
        category=button_id.removeprefix("filter_")
        if category not in FILTERS or category==self.category: return
        self.category=category
        for key in FILTERS:
            self.query_one(f"#filter_{key}",Button).set_class(key==category,"selected")
        for box in self.boxes.values(): box._last_text=None
        self.run_worker(self.refresh_data(),exclusive=True)

    def _scheduled_refresh(self):
        if not self.refresh_lock.locked(): self.run_worker(self.refresh_data(),exclusive=True)

    async def request_snapshot(self):
        reader,writer=await asyncio.open_unix_connection(SOCKET_PATH)
        try:
            writer.write(b"GET\n")
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
                box=BMCBox(host,self.display_name(host))
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
                    if box is not None: box.show_data(bmc,self.category)
                self.query_one("#status",Static).update(
                    f"Collector connected: {len(bmcs)} BMC(s) | View: {FILTERS[self.category]} | "
                    f"Refresh: {REFRESH_SECONDS:.1f}s | Pretty names: {'ON' if self.pretty_enabled else 'OFF'}"
                )
            except (FileNotFoundError,ConnectionRefusedError,OSError,json.JSONDecodeError) as e:
                self.query_one("#status",Static).update(f"Collector unavailable: {e}")

    def action_refresh_now(self):
        if not self.refresh_lock.locked(): self.run_worker(self.refresh_data(),exclusive=True)

if __name__=="__main__":
    args=parse_args()
    try:
        aliases=load_host_aliases(args.pretty_hosts_dir) if args.pretty_print else {}
    except (OSError,csv.Error) as e:
        raise SystemExit(f"Could not load pretty host aliases: {e}")
    IPMI_Cluster_Monitor(aliases,pretty_enabled=bool(args.pretty_print)).run()
