# EC440-Thread-Local-Storage
The fourth project for a Fall 2016 EC440 Operating Systems course at BU; Objective is to implement a local storage for threads.

Enviroment: Written in C and compiled with the attached Makefile. Should run on most/many unix distros.

Objectives: Implement local storage for threads that are created using the pthread library. The library must be able to create storage for a thread, write to it, read from it, delete it, and clone it. A thread's storage can not be accessed by any other thread without raising a segmentation fault.

Overview of implementation:
- Semaphores are used to protect critical sections of code from being interrupted.
- Memory is allotted using mmap and accessed by page.
- mprotect is used to enable or disable a page for writing or reading.
- Cloning is copy on write meaning if local storage is cloned, only the pages that are written to are copied. Otherwise, both local storages point to the same page. 
- It is possible for some pages in a local storage to belong to one thread while others do not. 
- Page faults are handled by checking to see why the fault was raised:
	- If it one thread was trying to access the storage of another thread, it is destroyed and the thread is exited.
	- If the fault was caused by some other problem, the signal is re-raised.

Testing:
- The main.c file for this project is only for testing the library.

Developed by Sam Beaulieu, see accompanying license for usage.


