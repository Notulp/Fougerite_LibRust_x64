import socket
import struct
import threading
import re
import customtkinter as ctk
from tkinter import ttk, messagebox

ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")


class RustLegacyRCON:
    def __init__(self, ip, port, password, log_callback, status_callback, disconnect_callback):
        self.ip = ip
        self.port = port
        self.password = password
        self.sock = None
        self.connected = False
        self.req_id = 1

        self.log_callback = log_callback
        self.status_callback = status_callback
        self.disconnect_callback = disconnect_callback

    def connect(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(5.0)
            self.sock.connect((self.ip, self.port))

            self.send_packet(3, self.password)

            while True:
                packet = self.read_packet()
                if not packet:
                    return False

                req_id, req_type, body = packet
                if req_type == 2:
                    if req_id == -1:
                        return False
                    else:
                        self.connected = True
                        threading.Thread(target=self.listen_loop, daemon=True).start()
                        return True
        except Exception as e:
            return False

    def send_packet(self, req_type, body):
        if self.sock:
            body_bytes = body.encode('utf-8')
            packet_size = 10 + len(body_bytes)
            packet = struct.pack('<iii', packet_size, self.req_id, req_type) + body_bytes + b'\x00\x00'
            self.sock.sendall(packet)
            self.req_id += 1

    def send_command(self, cmd):
        if self.connected:
            self.send_packet(2, cmd)

    def read_packet(self):
        try:
            size_data = b''
            while len(size_data) < 4:
                chunk = self.sock.recv(4 - len(size_data))
                if not chunk: return None
                size_data += chunk

            size = struct.unpack('<i', size_data)[0]

            payload = b''
            while len(payload) < size:
                chunk = self.sock.recv(size - len(payload))
                if not chunk: return None
                payload += chunk

            req_id, req_type = struct.unpack('<ii', payload[:8])
            body = payload[8:-2].decode('utf-8', errors='replace')
            return req_id, req_type, body
        except:
            return None

    def listen_loop(self):
        self.sock.settimeout(None)
        while self.connected:
            packet = self.read_packet()
            if not packet:
                self.connected = False
                self.disconnect_callback()
                break

            req_id, req_type, body = packet

            if req_type == 0 and body:
                self.log_callback(body)

                if "hostname:" in body and "players :" in body:
                    self.status_callback(body)

    def disconnect(self):
        self.connected = False
        if self.sock:
            try:
                self.sock.close()
            except:
                pass


class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Rust Legacy RCON - X64 Native")
        self.geometry("1000x650")
        self.rcon = None

        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        self.sidebar = ctk.CTkFrame(self, width=200, corner_radius=0)
        self.sidebar.grid(row=0, column=0, sticky="nsew")
        self.sidebar.grid_rowconfigure(5, weight=1)

        ctk.CTkLabel(self.sidebar, text="Server Connection", font=ctk.CTkFont(size=18, weight="bold")).grid(row=0,
                                                                                                            column=0,
                                                                                                            padx=20,
                                                                                                            pady=(
                                                                                                            20, 10))

        self.ip_entry = ctk.CTkEntry(self.sidebar, placeholder_text="127.0.0.1")
        self.ip_entry.insert(0, "127.0.0.1")
        self.ip_entry.grid(row=1, column=0, padx=20, pady=10)

        self.port_entry = ctk.CTkEntry(self.sidebar, placeholder_text="29016")
        self.port_entry.insert(0, "29016")
        self.port_entry.grid(row=2, column=0, padx=20, pady=10)

        self.pass_entry = ctk.CTkEntry(self.sidebar, show="*", placeholder_text="RCON Password")
        self.pass_entry.insert(0, "testing")
        self.pass_entry.grid(row=3, column=0, padx=20, pady=10)

        self.btn_connect = ctk.CTkButton(self.sidebar, text="Connect", command=self.toggle_connection)
        self.btn_connect.grid(row=4, column=0, padx=20, pady=20)

        self.status_label = ctk.CTkLabel(self.sidebar, text="Disconnected", text_color="red")
        self.status_label.grid(row=6, column=0, padx=20, pady=20)

        self.tabview = ctk.CTkTabview(self)
        self.tabview.grid(row=0, column=1, padx=20, pady=20, sticky="nsew")

        self.tab_console = self.tabview.add("Console")
        self.tab_players = self.tabview.add("Players")

        self.tab_console.grid_rowconfigure(0, weight=1)
        self.tab_console.grid_columnconfigure(0, weight=1)

        self.console_box = ctk.CTkTextbox(self.tab_console, state="disabled",
                                          font=ctk.CTkFont(family="Consolas", size=13))
        self.console_box.grid(row=0, column=0, columnspan=2, padx=10, pady=(10, 0), sticky="nsew")

        self.cmd_entry = ctk.CTkEntry(self.tab_console, placeholder_text="Enter command...")
        self.cmd_entry.grid(row=1, column=0, padx=(10, 0), pady=10, sticky="ew")
        self.cmd_entry.bind("<Return>", lambda e: self.send_command())

        self.btn_send = ctk.CTkButton(self.tab_console, text="Send", width=80, command=self.send_command)
        self.btn_send.grid(row=1, column=1, padx=10, pady=10)

        self.tab_players.grid_rowconfigure(1, weight=1)
        self.tab_players.grid_columnconfigure(0, weight=1)

        self.btn_refresh_players = ctk.CTkButton(self.tab_players, text="Refresh (status)",
                                                 command=self.refresh_players)
        self.btn_refresh_players.grid(row=0, column=0, padx=10, pady=10, sticky="w")

        self.players_count_label = ctk.CTkLabel(self.tab_players, text="Players: 0")
        self.players_count_label.grid(row=0, column=0, padx=10, pady=10, sticky="e")

        style = ttk.Style()
        style.theme_use("default")
        style.configure("Treeview", background="#2b2b2b", foreground="white", rowheight=25, fieldbackground="#2b2b2b",
                        borderwidth=0)
        style.map('Treeview', background=[('selected', '#1f538d')])
        style.configure("Treeview.Heading", background="#1f538d", foreground="white", relief="flat")
        style.map("Treeview.Heading", background=[('active', '#14375e')])

        columns = ("ID", "Name", "Ping", "Connected", "IP")
        self.tree = ttk.Treeview(self.tab_players, columns=columns, show="headings")

        for col in columns:
            self.tree.heading(col, text=col)
            self.tree.column(col, width=150, anchor="w")

        self.tree.grid(row=1, column=0, padx=10, pady=(0, 10), sticky="nsew")

    def toggle_connection(self):
        if self.rcon and self.rcon.connected:
            self.rcon.disconnect()
            self.on_disconnected()
        else:
            ip = self.ip_entry.get()
            port = int(self.port_entry.get())
            pwd = self.pass_entry.get()

            self.btn_connect.configure(text="Connecting...", state="disabled")
            self.update_idletasks()

            self.rcon = RustLegacyRCON(ip, port, pwd, self.on_log_received, self.on_status_received,
                                       self.on_disconnected)

            threading.Thread(target=self.do_connect, daemon=True).start()

    def do_connect(self):
        success = self.rcon.connect()
        self.after(0, self.connection_result, success)

    def connection_result(self, success):
        self.btn_connect.configure(state="normal")
        if success:
            self.btn_connect.configure(text="Disconnect")
            self.status_label.configure(text="Connected", text_color="green")
            self.ip_entry.configure(state="disabled")
            self.port_entry.configure(state="disabled")
            self.pass_entry.configure(state="disabled")
            self.append_console("[System] Successfully connected to RCON.")
            self.refresh_players()
        else:
            self.btn_connect.configure(text="Connect")
            messagebox.showerror("Connection Error", "Failed to connect or authentication rejected.")

    def on_disconnected(self):
        self.after(0, self._ui_disconnected)

    def _ui_disconnected(self):
        self.btn_connect.configure(text="Connect", state="normal")
        self.status_label.configure(text="Disconnected", text_color="red")
        self.ip_entry.configure(state="normal")
        self.port_entry.configure(state="normal")
        self.pass_entry.configure(state="normal")
        self.append_console("[System] Disconnected from server.")
        for item in self.tree.get_children():
            self.tree.delete(item)
        self.players_count_label.configure(text="Players: 0")

    def send_command(self):
        if self.rcon and self.rcon.connected:
            cmd = self.cmd_entry.get().strip()
            if cmd:
                self.append_console(f"> {cmd}")
                self.rcon.send_command(cmd)
                self.cmd_entry.delete(0, "end")

    def refresh_players(self):
        if self.rcon and self.rcon.connected:
            self.rcon.send_command("status")

    def on_log_received(self, text):
        self.after(0, self.append_console, text)

    def append_console(self, text):
        self.console_box.configure(state="normal")
        self.console_box.insert("end", text.strip() + "\n")
        self.console_box.see("end")
        self.console_box.configure(state="disabled")

    def on_status_received(self, status_text):
        self.after(0, self._parse_and_update_players, status_text)

    def _parse_and_update_players(self, status_text):
        for item in self.tree.get_children():
            self.tree.delete(item)

        lines = status_text.split('\n')

        for line in lines:
            line = line.strip()
            if line.startswith("players :"):
                match = re.search(r"players\s*:\s*(\d+)", line)
                if match:
                    self.players_count_label.configure(text=f"Players: {match.group(1)}")

            match = re.match(r"^(\d{10,20})\s+(.+?)\s+(\d+)\s+([\ds]+)\s+([\d\.:]+)$", line)
            if match:
                steam_id = match.group(1)
                name = match.group(2).strip('"')
                ping = match.group(3)
                conn_time = match.group(4)
                ip = match.group(5)

                self.tree.insert("", "end", values=(steam_id, name, ping, conn_time, ip))


if __name__ == "__main__":
    app = App()
    app.mainloop()