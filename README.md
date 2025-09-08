# whatspace
Utilities for determining the actual capacity of a USB drive.

## Background
I came across a few high capacity USB drives, online, that were inexpensive - $25 each. The drives were 30 TB, 128 TB and 256 TB. As much I can dream of owning 1 PB of disk space for ~$200, I understood going in that these drives would be fakes. I wanted to find out what the exact capacity of these drives were, how they failed and whether I could use them to their true capacity.

## The Drives
Here's a picture of the 30 TB, 128 TB and a 128 GB control drive I used for testing:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/2a567d26-b7e3-4e64-bad4-de25686878fa" />

Note that I bricked the 256 TB drive, and recycled it, prior to me deciding to publish my results. That's why it missing from the picture. All the drives are off brand, and I have had the control USB drive for a while, so I know it works.

## The Spiel
I purchased the drives 30 TB, 128 TB and 256 TB drives from 3 different vendors. However, they all had the same directions on their product page:

1. The drive could only be formatted on Windows.
2. The drive must be formatted with exFAT.
3. The drive must not be formatted on Linux or macOS.
4. The drive should not be used in a game console.

## Who Needs Instructions?
I decided to ignore the instructions and started using the 256 TB drive on Linux. I am more comfortable with the Linux environment than Windows. Putting aside the requirement for exFAT, I decided that any device should be able to accept a seek, write and read request. So, I wrote a quick program in C that opened a block device (O_DIRECT to avoid any file system cache) and used lseek64() to move the file pointer to a location on disk (1 GiB blocks). The program used write() to output a unique 4 KiB data buffer to the block device and then used read() to read the data back. If there was a write or read error, or a mismatch between the write and read buffer, the current 1 GiB offset was output to show the capacity of the device.

I was able to run this utility against the 256 GB drive, which presented itself as 2 x 128 TB drives. The actual size of both of the drives was ~72 GiB. 

Next, I tried to format one of the drives using ext4. This was a complete failure. I then moved the drive over to Windows, which was unable to format the drive with exFAT. In fact, Windows could not even bring the drive online to initialize it. This was the result of my experimental utility, and I decided to recycle the drive.

I still have the code for the utility, but decided not to publish it in the repository. The utility is not robust, and very dangerous if run on the wrong block device. After trying to write a disclaimer, it became apparent to me it would be better to not publish the code.

## I Need Instructions
After bricking one drive with my ethusiasm, I decided I should follow the vendor's instructions and moved over to testing on Windows.

I used Visual Studio 2022 Community Edition to create two C projects - spacechk and maxspace. The utilities approached the problem in two different ways:

1. The spacechk utility creates a series of files to fill the available disk space.
2. The maxspace utility creates one file that is extended to fill the available disk space.

## The spacechk Utility

The spacechk utility does the following:

1. The utility has separate options for file creation, verification, and deletion.
       
2. When run with the creation option, the utility creates a series of 10 MiB files with a unique pattern that will fill the available space on the drive. If the file creation fails, the disk offset of the file is reported and that’s the total capacity of the drive. 
       
3. When run with the verification option, the utility will read back the created files and verify the unique pattern. If the pattern fails to validate, the disk offset of the file is reported and that’s the total capacity of the drive.
       
4. The deletion phase just deletes the created files to clean up the disk space.

After running this utility, I found it had a couple of flaws. The main flaw is described in the following diagram:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/6604c46d-1b56-48a7-9c03-8f572ff2f155" />

Files are created in sequence number order e.g. sp000000.bin through spffffff.bin. The sequence numbers are hex to pack as much information into an 8.3 format file name as possible. The utility uses 10 MiB sized files, and so a file with sequence number 0x1000 would mean ~40 MiB disk space has been used. However, placement of the actual data is left to the OS and device. So, it is quite possible for a file with a sequence number of 0x1 to be written to a disk offset of 20 GiB. 

The other flaw with this method is the way files are located for verification. The verification phase uses the FindFirstFile() and FindNextFile() APIs which are not guaranteed to return files in any specific order. This issue could be corrected by using the same filename code as the creation phase, but it was clear that the spacechk utility was not a good way to determine the exact capacity of a drive.

## The maxspace Utility

The maxspace utility was very close to my original Linux code and does the following:
       
1. Uses a file seek size of 10 MiB.
       
2. Quickly creates a very large file that takes up space of the entire drive.
       
3. Starts a loop, using 0 as an initial file offset.
       
4. Writes a 4 KiB buffer to the disk with a unique pattern.
       
5. Reads the unique pattern back. 
       
6. Moves the file offset to the next 10 MiB block.
       
7. Runs until there is a file write or read error, or the unique pattern is not correct. 
       
8. Reports the file offset where the error occurred.

Windows has a sparse file capability, but that is not available on exFAT volumes. To quickly create a large file on exFAT you have to:

1. Run the utility as Administrator. This is known as running at an elevated privilege. I was not thrilled about this, but it’s the only way I was able to achieve the goal.
       
2. The program has to obtain the SE_MANAGE_VOLUME_NAME privilege.
       
3. Determine the amount of available space on the drive.
       
4. Create a new file using the CreateFile() API (https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).
       
5. Use the SetFilePointerEx() API (https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfilepointerex) to move the file pointer to the amount of space on the drive.
       
6. Use the SetEndOfFile() API (https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setendoffile) to create a new end of file marker.
       
7. Use the SetFileValidData() API (https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfilevaliddata) call to prevent Windows from writing zeroes from the start of the file to the full size of the file.

The following diagram depicts a potential security issue with this method:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/c5830e0f-ffb8-4e5a-8550-b60197b6c820" />

This is a lot to take in, but worth understanding. To make certain disk operations faster like formatting and file deletion, very little information is written to disk. In the case of file deletion, the OS metadata about the file e.g. name, location on disk etc. is updated with a deleted status and the disk space belonging to the file is marked as free. The file data is still there. As a security feature, Windows forces zeroes into parts of a file that are not initialized e.g. have not been written to. This prevents old data being read from the disk. 

The behavior we want bypasses this security feature. We want to create a large file with no data in it. We don’t want to inspect the original content of the disk, we just want a large file that we can use to determine the actual capacity of the drive.

The default Windows behavior also hinders us in a couple of other ways. Writing zeroes to the file to fill 32 TB or 128 TB takes a lot of time, and will not report exactly where any failure occurs.

The maxspace utility presents an additional challenge:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/09d8f7e7-8ed4-4105-96de-2db2b7ed393c" />

Although we want to scan the disk as quickly as possible, choosing a very large block size may led to a false failure. For example, using a 1 TiB block size allows would only take 30 samples of the 32 TB drive (30 TiB formatted). If the drive reported a failure at 1 TiB, we might incorrectly assume the capacity was 1 TiB. If we use 1 MiB blocks, we have more blocks to scan which take longer, but may present an accurate picture of capacity. Through some experimentation, I ended up picking 10 MiB as a block size. This gave a balance between performance, accuracy and drive stability.

## File System Caching
Both the maxspace and spacechk utilities use files that bypass the Windows file system cache. This is important as we don’t want the utilities to read data that might be cached and give a false capacity reading. This is why the utilities use unique pattern buffer that are 4 KiB. The drives use sector sizes of 4 KiB (4096 bytes) and bypassing the file system cache requires buffers to be aligned on a sector boundary, and be a multiple of the sector size, otherwise the file transfer will fail.

Just out of interest, I added an option to the spacechk utility to allow caching to see if that changed the result.

## The Time Factor
Being able to determine the true capacity of a drive quickly was one of the major goals of this project. 
It is very easy to write a program, or use a utility like dd on Linux, to create a file and start writing data. When the file can’t take any more data, the true size of the device has been found.

The maximum file size for exFAT is 16 exabytes (EB), so creating a 128 TiB file should not be an issue for this file system. However, the following chart shows why this might not work:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/c960ed6f-59a0-4ad2-a723-3feea801baec" />

If we had a transfer rate of 200 MiB/s to the drive, it would take ~187 hours to write a 128 TiB file, which is ~8 days. Realistically, the transfer rate will be closer to the 10 MiB/s to 50 MiB/s range which would put the completion time at 155 to 32 days.

Even at 8 days, plenty can go wrong to get in the way of the test. In fact, while I was running some of these tests I had some power outages. Thankfully, I was running off a laptop and did not have to restart the tests.

## The Control Test
As with any good test, there has to be a control. For the capacity testing, I used an off brand USB drive I have had for a while which I know works. The drive is 128 GB and 117 GiB after formatting.

Here’s the Windows Explorer properties for the drive:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/868901da-50d5-4e76-b81f-60cd64492082" />

Here are the results of the spacechk utility:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/61905fd5-79ab-45ee-af99-ab22dd6fce86" />

Here are the results of the maxspace utility:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/8c93a818-ed47-4f84-bff3-c0a98a1e4312" />

As expected, the drive capacity was reported as 117 GiB by both utilities. The spacechk utility took a little over 4 hours for the creation, verification, and deletion phases. The maxspace utility only took ~3 minutes.

## The 32 TB Test
The 32 TB drive is ~30 TiB once formatted:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/24627ca7-e20d-4bc2-bbf6-1434990c0012" />

Running the spacechk test produced some interesting (alarming) results:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/2b0e1b63-0f20-402a-b024-b29f85a3da17" />

After ~7 hours, I decided to spot check one of the files which was sp-5900.bin. The file was all zeroes which I knew was incorrect as there should have been a sequence number in the first 8 bytes of the file. The creation test had managed to create 23,275 files and took ~227 MiB disk space.

I stopped the creation test and ran the verification test which took ~46 minutes and failed at ~36 GiB. This is likely a result of the file content being placed at an invalid disk location, but the file sequence number being fairly low.

You will also note that this test was going to create ~3.2M files. The exFAT file system can only store ~2.7M files in one folder. I was not too concerned about this limit as I expected drives to report a failure prior to that limit being reached.

I moved onto the maxspace test:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/118a35c2-3e81-41cc-b748-3571f83b4554" />

In the last screenshot, you can see the size of the test file which is 30 TiB. The test that bypassed the Windows file system cache took ~1.75 hours and failed at 53 GiB. The test that used the Windows file system cache took ~3 hours and did not fail at all.

## The 128 TB Test

The 128 TB drive is ~122 TiB after formatting:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/2babb53d-4836-46d3-a867-98368061487e" />

Here are the results of the spacechk test:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/0611ee21-044b-4800-a126-264f6b2ce300" />

The creation test failed after ~36 minutes at ~22 GiB. This is likely a result of the file content being placed at an invalid disk location, but the file sequence number being fairly low.

Here’s the maxspace test:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/0f7359c5-b711-44dd-ac40-80bcd449aab1" />

With the Windows file system cache bypassed, the test took ~20 seconds and indicated the drive was ~48 GiB. With the file system cache, the test took ~13 seconds and the drive capacity was reported as ~71 GiB.

## Conclusion

The spacechk utility is a lot more accurate than the maxspace utility, at the cost of requiring Administrator rights.

The 32 TB drive, under normal conditions e.g. working with the Windows file system cache, failed in the worst way possible. This drive just ate data. You could copy a file to this drive and be unlucky enough to read the version in cache back, which would give you a false sense of security.

At least the 128 TB drive failed quickly and advertised the failure. 

Each drive has a real capacity that is significantly below the advertised size – 0.157% and 0.0374% respectively.

The drives turned out to be highly unstable, which means I won’t be using them to their actual capacity. They would just disappear from Windows. On a few occasions, I had to reboot Windows to get the drives to be recognized again.

## Other Tools

The Sysinternals suite includes a contig utility (https://learn.microsoft.com/en-us/sysinternals/downloads/contig) which can quickly create a file with no data it in. This must be run as Administrator. Once the file is created, another program could be written to test random blocks in the file.

In researching this topic, I ran across a utility called ValiDrive (https://www.grc.com/validrive.htm). It looks like a really capable tool and I may try it out. 

I could have used either tool, but the purpose of this project was to write some C code to better understanding the challenges involved when determining drive capability.

## How to Build the Utilities
The Visual Studio 2022 projects are included in the src/maxspace and src/spacechk directories. The projects can be download and compiled using VS2022.

## How to Run the spacechk Utility
The spacechk utility can be run from a regular Windows Command Prompt. Just running the command without any options will display a list of command line options. Options can be combined, but I ran the tests as follows (file creation):

       spacechk -create e:\

Then verification:

       spacechk -verify e:\

And finally deletion:

       spacechk -delete e:\

I could have run:

       spacechk -create -verify -delete e:\

But, I was still tweaking the code and running separate commands was useful.

The utility has a -stats option which will output the sector size, number of clusters, total space and available space of the drive.

## How to Run the maxspace Utility
The maxspace utility needs an elevated Windows Command Prompt. This means you have to right mouse click on the Command Prompt icon and select "Run as Administrator".

The utility can be run one of two ways. The default command line just needs the drive of the USB device which will bypass the Windows file system cache:

       maxspace e:\

To use the file system cache:

       maxspace -cache e:\

The utility has a -stats option which will output the sector size, number of clusters, total space and available space of the drive.

## Next Steps

There look to be some Windows mechanisms for reserving large contiguous files when defragmenting a drive. The process is discussed here https://learn.microsoft.com/en-us/windows/win32/fileio/defragmenting-files at a very high level. It might be worth seeing if the information can be used to improve the file creation process in the spacechk utility.
