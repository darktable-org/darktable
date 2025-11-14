#!/usr/bin/env python3

"""
This script automatically enables Discord Rich Presence whenever darktable is running and clears it when darktable closes.

How to Run:

Open a terminal where this script is

Install the required python package:
pip install psutil pypresence

Run the script:
python darktable_rpc.py

Keep the terminal running when you launch darktable, your Discord status will update automatically.
"""
import time
import psutil
from pypresence import Presence

# Discord Application ID
CLIENT_ID = '1437906050467631326'

PROCESS_NAMES = ['darktable', 'darktable.exe', 'darktable-bin']

def is_darktable_running():
    for proc in psutil.process_iter(['name']):
        try:
            if proc.info['name'] in PROCESS_NAMES:
                return True
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return False

def main():
    print("darktable Discord Rich Presence")
    print("Waiting for darktable to start...")
    
    rpc = None
    presence_active = False
    start_time = None
    
    try:
        while True:
            darktable_running = is_darktable_running()
            
            # darktable just started
            if darktable_running and not presence_active:
                print("darktable detected, activating Discord presence...")
                
                try:
                    rpc = Presence(CLIENT_ID)
                    rpc.connect()
                    
                    start_time = int(time.time())
                    
                    rpc.update(
                        state="Photo editing",
                        details="darktable",
                        large_image="darktable_icon",
                        large_text="darktable - photography workflow",
                        start=start_time
                    )
                    
                    presence_active = True
                    print("Discord presence active!")
                    
                except Exception as e:
                    print(f"Error connecting to Discord: {e}")
                    print("Make sure Discord is running.")
            
            # darktable just closed
            elif not darktable_running and presence_active:
                print("darktable closed. Clearing Discord presence...")
                
                try:
                    if rpc:
                        rpc.close()
                        rpc = None
                except Exception as e:
                    print(f"Error closing Discord connection: {e}")
                
                presence_active = False
                start_time = None
                print("Waiting for darktable to start again...")
            
            time.sleep(5)
            
    except KeyboardInterrupt:
        if rpc and presence_active:
            try:
                rpc.close()
            except:
                pass
        print("Discord Rich Presence stopped.")

if __name__ == "__main__":
    main()
