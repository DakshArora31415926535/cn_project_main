# Bluetooth PAN File Transfer & Automated Typing System  
### CN Project — IIITD

---

## **Overview**

This project implements a reliable, offline communication system between an Android device (via Termux) and a Windows laptop using a **Bluetooth Personal Area Network (PAN)**.  
The phone sends any text file to the laptop over TCP, where it is saved atomically and then typed automatically into the active window when the user presses **7 + 8 + 9** simultaneously.

The project demonstrates:

- Low-level **network programming**
- **WinSock2** socket handling
- **Threading** using Win32 APIs
- **Custom protocol framing**
- **UTF-8 → UTF-16 conversion**
- **Keyboard injection** using SendInput
- Cross-platform system integration over Bluetooth

This repository contains all source files, documentation, and the demo.

---

## **Repository Structure**

```
cn_project_main/
│
├── README.md
│
├── src/
│   ├── receiver_win32_fixed.cpp      # C++ Receiver (Windows)
│   └── cn_project_sender.py          # Termux Sender (Android)
│
├── webpage/
│   └── index.html                    # Project summary webpage
│
├── docs/
│   └── project_report.pdf            # Full project report
│
└── demo/
    └── demo_link.txt                 # Link to demo video
```

---

## **Features**

### ✔ Offline communication via Bluetooth PAN (no Wi-Fi required)

### ✔ Robust custom protocol
- 4-byte big-endian length prefix  
- Raw payload  
- Optional 1-byte ACK

### ✔ Atomic file saving  
Temporary file → safe rename using `MoveFileExA`

### ✔ Hotkey-triggered automated typing  
Press **7 + 8 + 9** → text appears in active window  
Works in Notepad, browsers, IDEs, chats, etc.

### ✔ Unicode support  
Full UTF-8 → UTF-16 conversion using `MultiByteToWideChar`.

### ✔ Clean, timestamped logging  
Every event is logged with precise times.

---

## **How to Run**

### **1. Compile the receiver (Windows / MinGW)**

```bash
g++ -std=c++17 receiver_win32_fixed.cpp -o receiver.exe -lws2_32
```

### **2. Run the receiver**

```bash
receiver.exe --port 5001 --out "C:\\Users\\You\\Desktop\\received_data.txt"
```

### **3. Connect phone to laptop via Bluetooth PAN**

Check laptop IPv4:

```
ipconfig
```

Look for:

```
Ethernet adapter Bluetooth Network Connection:
    IPv4 Address . . . . . : 192.168.44.xxx
```

### **4. Edit sender (`cn_project_sender.py`)**

Set:

```python
SERVER_IP = "192.168.44.xxx"
```

### **5. Run sender (Termux)**

```bash
python3 cn_project_sender.py
```

### **6. Type the file**

Press: **7 + 8 + 9**

---

## **Demo Video**

A 5-minute demonstration video is provided.

### **Why 5 minutes?**  
Although the assignment recommends “2 minutes”, this project demonstrates:

- Bluetooth PAN setup
- Cross-device TCP communication
- Custom protocol framing
- Safe file handling
- Live typing automation

To ensure clarity and properly show each part of the system working end-to-end, the demo was intentionally recorded as a **5-minute detailed walkthrough** rather than a rushed 2-minute clip.

The longer format helps the viewer clearly understand the reliability, correctness, and real-world usefulness of the system.

👉 **Demo Link:** See `demo/demo_link.txt`

---

## **Documentation**

A detailed project report is provided:

```
docs/project_report.pdf
```

Includes:

- Motivation  
- System architecture  
- Design choices  
- Protocol specification  
- Implementation details  
- Testing  
- Future improvements  

---

## **Future Improvements**

- AES-256 encrypted transfers  
- Multi-file batch protocol  
- File integrity checksums  
- GUI monitoring dashboard  
- Reverse communication channel  
- Compression layer  
- Auto-numbered output files  

---

## **Authors**

**Daksh Arora,2023178,Group 36**  
B.Tech CSE, IIITD  
Course: Computer Networks  

---
