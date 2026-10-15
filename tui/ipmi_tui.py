#!/usr/bin/env python3
import asyncio,json,time
from textual.app import App,ComposeResult
from textual.containers import Horizontal,VerticalScroll
from textual.widgets import Footer,Header,Static

SOCKET_PATH="/tmp/ipmi_cluster_monitor.sock"
REFRESH_SECONDS=0.5

def widget_id(host): 
    return "bmc_"+"".join(c if c.isalnum() else "_" for c in host)

class BMCBox(Static):
    def __init__(self,host):
        super().__init__(id=widget_id(host))
        self.host=host
        self.border_title=host
        self._last_text=None

    def show_data(self,bmc):
        age=max(0.0,time.time()-float(bmc.get("timestamp",0)))
        status="ONLINE" if age<5.0 else f"OFFLINE since {age:.1f}s"
        lines=[
            f"Status: {status}   Logging: {'ON' if bmc.get('logging') else 'OFF'}",
            #f"Sample: {bmc.get('generation',0)}   Age: {age:.2f}s",
            f"Sample: {bmc.get('generation',0)}",
            ""
        ]
        for s in bmc.get("sensors",[]):
            lines.append(f"{s.get('name','')[:24]:<24} {s.get('value',0):>10.3f} {s.get('unit','')}")
        text="\n".join(lines)

        # vis-jitter: Do not repaint the widget when the displayed text has not changed.
        if text != self._last_text:
            self.update(text)
            self._last_text=text

class IPMI_Cluster_Monitor(App):
    CSS="""
    Screen{layout:vertical}
    #status{height:1;padding:0 1}
    #servers{height:1fr;width:100%;padding:1}
    .bmc-row{width:100%;height:auto;min-height:10}
    BMCBox{width:1fr;height:auto;min-height:10;margin:0 1;border:round $accent;padding:0 1}
    """

    BINDINGS=[("q","quit","Quit"),("r","refresh_now","Refresh")]

    def __init__(self):
        super().__init__()
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
        # A refresh is allowed to run only if the previous one is finished, atleast thats what I think is going on.
        if not self.refresh_lock.locked():
            self.run_worker(self.refresh_data(),exclusive=True)

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
        new_hosts=[bmc.get("host","unknown") for bmc in bmcs]
        old_scroll=servers.scroll_y

        # Layout is rebuilt only when the set/order of BMCs changes.
        await servers.remove_children()
        self.boxes={}
        self.host_order=new_hosts

        for i in range(0,len(bmcs),2):
            row=Horizontal(classes="bmc-row")
            await servers.mount(row)
            for bmc in bmcs[i:i+2]:
                host=bmc.get("host","unknown")
                box=BMCBox(host)
                self.boxes[host]=box
                await row.mount(box)

        await asyncio.sleep(0)
        servers.scroll_y=old_scroll

    async def refresh_data(self):
        if self.refresh_lock.locked():
            return

        async with self.refresh_lock:
            try:
                data=await self.request_snapshot()
                bmcs=data.get("bmcs",[])
                hosts=[bmc.get("host","unknown") for bmc in bmcs]

                if hosts != self.host_order:
                    await self.build_layout(bmcs)

                for bmc in bmcs:
                    host=bmc.get("host","unknown")
                    box=self.boxes.get(host)
                    if box is not None:
                        box.show_data(bmc)

                self.query_one("#status",Static).update(
                    f"Collector connected: {len(bmcs)} BMC(s) | "
                    f"Refresh: {REFRESH_SECONDS:.1f}s | Socket: {SOCKET_PATH}"
                )

            except (FileNotFoundError,ConnectionRefusedError,OSError,json.JSONDecodeError) as e:
                self.query_one("#status",Static).update(f"Collector unavailable: {e}")

    def action_refresh_now(self):
        if not self.refresh_lock.locked():
            self.run_worker(self.refresh_data(),exclusive=True)

if __name__=="__main__":
    IPMI_Cluster_Monitor().run()
