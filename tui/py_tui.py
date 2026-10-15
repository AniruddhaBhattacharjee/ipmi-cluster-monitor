import textual.app
import textual.widgets

class CalculatorApp(textual.app.App):
   def compose(self):
       yield textual.widgets.Header()
       yield textual.widgets.Footer()
       yield textual.widgets.Static("Calculator")
   def on_mount(self):
       self.set_interval(1, self.update_time)
   def update_time(self):
       self.query_one(textual.widgets.Static).update("Calculator")

if __name__ == "__main__":
   CalculatorApp.run()