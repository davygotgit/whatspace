//	Check how much disk space a device actually has. This is
//	done by creating files with a specific pattern, until there
//	is no more space on the disk, and then reading the files back
//	to verify the data was actually written
//

#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

#include <chrono>
#include <filesystem>

//	Size metrics e.g. KiB, GiB etc.
constexpr uint64_t KiB = 1024;
constexpr uint64_t MiB = KiB * 1024;
constexpr uint64_t GiB = MiB * 1024;
constexpr uint64_t TiB = GiB * 1024;

//	Converts bytes to human readable sizes
constexpr uint64_t			sizeArray []	= {TiB, GiB, MiB, KiB};
constexpr const wchar_t*	sizeNames []	= {L"TiB", L"GiB", L"MiB", L"KiB"};
constexpr const wchar_t*	byteName		= L"bytes";
constexpr int				numSizes		= sizeof(sizeArray) / sizeof(sizeArray[0]);

//	File prefix
constexpr const wchar_t*	filePrefix		= L"sp";

//	File I/O size
constexpr uint64_t			fileIOSize		= 10 * MiB;

//	Batch size for some operations
constexpr uint64_t			batchSize		= 10;

//	Program actions
namespace checkActions
{
	uint8_t noActions		= 0;
	uint8_t outputStats		= 1;
	uint8_t createFiles		= 2;
	uint8_t verifyFiles		= 4;
	uint8_t keepVerifying	= 8;
	uint8_t deleteFiles		= 16;
};


//	Output an error message
void PrintError(const wchar_t* format, ...)
{
	//	We start by saving the current error as we might make
	//	API calls that produce other errors
	auto savedError = GetLastError();

	//	There are two parts to the error message. There's the Windows
	//	description of the error and information passed by the user.
	//	Start by getting the Windows error text
	LPCTSTR windowsMsg = nullptr;

	//	Format the error message
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, savedError,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&windowsMsg, 0, nullptr);

	//	User message
	wchar_t userMsg[BUFSIZ];

	//	Get the start of the variable arguments
	va_list ourArgs;
	va_start(ourArgs, format);
	vswprintf_s(userMsg, format, ourArgs);

	//	Output the full message
	wprintf(L"%s : %s\n", userMsg, windowsMsg);

	//	Free off the Windows message buffer
	LocalFree((LPVOID)windowsMsg);
}


//	Output a human readable size
const wchar_t* HumanReadable (uint64_t sizeInBytes, uint64_t &convertedSize)
{
	for (int i = 0; i < numSizes; i ++)
	{
		if (sizeInBytes >= sizeArray [i])
		{
			convertedSize = sizeInBytes / sizeArray [i];
			return sizeNames [i];
		}
	}

	//	Must be in bytes
	convertedSize = sizeInBytes;
	return byteName;
}


//	Common output function for sizes
void OutputSize (const wchar_t* msg, const uint64_t inSize)
{
	uint64_t converted;
	const wchar_t* textSize = HumanReadable(inSize, converted);
	wprintf(L"%s %lld %s\n", msg, converted, textSize);
}


//	Find any previous files we created, so we can skip
//	over them
uint64_t FindPriorFiles (const char* pathName)
{
	//	Create the search path
	wchar_t searchPath [MAX_PATH];
	swprintf_s(searchPath, L"%hs%s*.bin", pathName, filePrefix);

	WIN32_FIND_DATA findData;
	HANDLE findHandle = FindFirstFile(searchPath, &findData);
	if (findHandle == INVALID_HANDLE_VALUE)
	{
		//	This does not mean there's a real error - start at
		//	the first file
		return 0;
	}

	uint64_t maxSeq = 0;
	do
	{
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			//	Get the sequence number from the filename
			wchar_t* seqPtr = wcschr(findData.cFileName, '-');
			if (seqPtr == nullptr)
			{
				//	Something is wrong - use the max sequence number we have
				FindClose(findHandle);
				return maxSeq;
			}

			//	Get the sequence number from the file name
			uint64_t seqNum;
			swscanf_s(seqPtr + 1, L"%llx", &seqNum);

			//	Set the maximum sequence number found
			maxSeq = max(seqNum, maxSeq);
		}

	} while (FindNextFile(findHandle, &findData));

	FindClose(findHandle);

	return maxSeq;
}


//	Create a number of files on the device
bool CreateFiles (const char* pathName, const DWORD bytesPerSector, const uint64_t totalSpace)
{
	//	Work out how many files we will create
	uint64_t totalFiles = totalSpace / fileIOSize;

	//	Output some information
	wprintf(L"\nI will create %lld files ", totalFiles);
	OutputSize(L" with size ", fileIOSize);

	//	We will be using I/O that bypasses the file system cache which means our
	//	buffers need to be aligned on a sector boundary
	uint8_t* writeBuffer = (uint8_t *) _aligned_malloc(fileIOSize, bytesPerSector);
	if (writeBuffer == nullptr)
	{
		PrintError(L"Could not get write buffer");
		return false;
	}

	//	Clear out the buffer
	memset(writeBuffer, 0, fileIOSize);

	//	Get a start time
	auto start		= std::chrono::high_resolution_clock::now();
	auto elapsed	= start;

	//	Find previous files to skip
	uint64_t startFile = FindPriorFiles(pathName);

	//	Create all files
	wchar_t writeName [MAX_PATH];
	for (uint64_t i = 0; i < totalFiles; i++)
	{
		//	Output some stats if it is time
		if (i && i % batchSize == 0)
		{
			//	Get the current time
			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> batchSeconds		= end - start;
			std::chrono::duration<double> elapsedSeconds	= end - elapsed;

			//	Inform the user
			printf("\r%lld/%lld written took %.2lf seconds (%.2lf seconds total)   ", i, totalFiles, batchSeconds.count(), elapsedSeconds.count());

			//	Reset the batch timer
			start = std::chrono::high_resolution_clock::now();
		}

		//	Create the filename
		swprintf_s(writeName, L"%hs%s%06llx.bin", pathName, filePrefix, i);

		//	Create the file
		HANDLE writeFile = CreateFile(writeName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (writeFile == INVALID_HANDLE_VALUE)
		{
			PrintError(L"Cannot create file %s\n", writeName);
			_aligned_free(writeBuffer);
			return false;
		}

		//	Write unique data into the file
		uint64_t dataOffsets = fileIOSize / 4;
		for (int o = 0; o < 4; o++)
		{
			uint64_t* dataPtr = (uint64_t*)(writeBuffer + (o * dataOffsets));
			*dataPtr = i + 1;
		}

		//	Write the data
		DWORD written;
		if (WriteFile(writeFile, writeBuffer, fileIOSize, &written, nullptr) == 0)
		{
			PrintError(L"\nCannot write to %s\n", writeName);
			OutputSize(L"Reached", i * fileIOSize);
			_aligned_free(writeBuffer);
			return false;
		}

		//	Sanity check
		if (written != fileIOSize)
		{
			PrintError(L"Wrote %d bytes, expected %lld bytes\n", written, fileIOSize);
			_aligned_free(writeBuffer);
			return false;
		}

		//	Close this file
		CloseHandle(writeFile);
	}

	//	We can free off the buffer
	_aligned_free(writeBuffer);

	//	Output some information
	wprintf(L"\nWrote %lld total files ", totalFiles);
	OutputSize(L"taking", totalFiles * fileIOSize);

	//	All good
	return true;
}


//	Verify that data we wrote to the device made it
bool VerifyFiles (const char* pathName, const DWORD bytesPerSector, const bool keepGoing)
{
	//	Create the search path
	wchar_t searchPath [MAX_PATH];
	swprintf_s(searchPath, L"%hs%s*.bin", pathName, filePrefix);

	WIN32_FIND_DATA findData;
	HANDLE findHandle = FindFirstFile(searchPath, &findData);
	if (findHandle == INVALID_HANDLE_VALUE)
	{
		PrintError(L"Unable to find %s files", searchPath);
		return false;
	}

	//	We will be using I/O that bypasses the file system cache which means our
	//	buffers need to be aligned on a sector boundary
	uint8_t* verifyBuffer = (uint8_t*) _aligned_malloc(fileIOSize, bytesPerSector);
	if (verifyBuffer == nullptr)
	{
		PrintError(L"Could not get verify buffer");
		return false;
	}

	//	Output some information
	wprintf(L"Starting verification stage\n");

	//	Get a start time
	auto start		= std::chrono::high_resolution_clock::now();
	auto elapsed	= start;

	//	Read and verify the files
	uint64_t count = 0;
	do
	{
		if (count && count % batchSize == 0)
		{
			//	Get the current time
			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> batchSeconds		= end - start;
			std::chrono::duration<double> elapsedSeconds	= end - elapsed;

			//	Inform the user
			wprintf(L"\rTotal verifications %lld, last %lld verifications took %.2lf seconds (%.2lf total seconds)   ", count, batchSize, batchSeconds.count(), elapsedSeconds.count());

			//	Reset the batch timer
			start = std::chrono::high_resolution_clock::now();
		}

		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			//	The file will be missing the path name
			wchar_t verifyName [MAX_PATH];
			swprintf_s(verifyName, L"%hs%s", pathName, findData.cFileName);

			//	Open the file
			HANDLE verifyFile = CreateFile(verifyName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
			if (verifyFile == INVALID_HANDLE_VALUE)
			{
				PrintError(L"\nCannot open file %s", verifyName);
				_aligned_free(verifyBuffer);
				return false;
			}

			//	Read the data
			DWORD bytesRead;
			if (ReadFile(verifyFile, verifyBuffer, fileIOSize, &bytesRead, nullptr) == 0)
			{
				PrintError(L"\nCannot read from %s", verifyName);
				_aligned_free(verifyBuffer);
				CloseHandle(verifyFile);
				return false;
			}

			//	Close the file
			CloseHandle(verifyFile);
			verifyFile = INVALID_HANDLE_VALUE;

			//	Sanity check
			if (bytesRead != fileIOSize)
			{
				PrintError(L"\nRead %d bytes, expected %lld bytes, error 0x%X", bytesRead, fileIOSize);
				_aligned_free(verifyBuffer);
				return false;
			}

			//	Get the sequence number from the filename
			wchar_t* seqPtr = wcschr(verifyName, '-');
			if (seqPtr == nullptr)
			{
				wprintf(L"\nCould not find sequence number from %s", verifyName);
				_aligned_free(verifyBuffer);
				return false;
			}

			uint64_t seqNum;
			swscanf_s(seqPtr + 1, L"%llx", &seqNum);

			//	Make sure our unique data is in the file
			uint64_t dataOffsets = fileIOSize / 4;
			for (int o = 0; o < 4; o++)
			{
				uint64_t* dataPtr = (uint64_t*) (verifyBuffer + (o * dataOffsets));
				if (*dataPtr != seqNum + 1)
				{
					printf("\nData buffer should be 0x%llX @ offset 0x%llX, but is 0x%llX\n", seqNum + 1, o * dataOffsets, *dataPtr);
					OutputSize(L"Reached", (seqNum + 1) * fileIOSize);

					if (!keepGoing)
					{
						//	We can stop
						_aligned_free(verifyBuffer);
						return false;
					}
				}
			}

			//	Number of files we verified
			count ++;
		}
	} while (FindNextFile(findHandle, &findData));

	FindClose(findHandle);

	//	We can free off the buffer
	_aligned_free(verifyBuffer);

	//	Output some information
	wprintf(L"\nVerified %lld total files", count);
	OutputSize(L"taking", count * fileIOSize);

	//	All good
	return true;
}


//	Delete files we created
bool DeleteFiles (const char* pathName)
{
	//	Create the search path
	wchar_t searchPath [MAX_PATH];
	swprintf_s(searchPath, L"%hs%s*.bin", pathName, filePrefix);

	WIN32_FIND_DATA findData;
	HANDLE findHandle = FindFirstFile(searchPath, &findData);
	if (findHandle == INVALID_HANDLE_VALUE)
	{
		PrintError(L"Could not locate %s files to delete", searchPath);
		return false;
	}

	//	Output some information
	wprintf(L"\nDeletion phase starting\n");

	//	Get a start time
	auto start		= std::chrono::high_resolution_clock::now();
	auto elapsed	= start;

	uint64_t count = 0;
	do
	{
		if (count && count % batchSize == 0)
		{
			//	Get the current time
			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> batchSeconds		= end - start;
			std::chrono::duration<double> elapsedSeconds	= end - elapsed;

			//	Inform the user
			printf("\rTotal deletions %lld, last %lld deletions took %.2lf seconds (%.2lf total seconds)   ", count, batchSize, batchSeconds.count(), elapsedSeconds.count());

			//	Reset the batch timer
			start = std::chrono::high_resolution_clock::now();
		}

		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			//	The file will be missing the path name
			wchar_t deleteName [MAX_PATH];
			swprintf_s(deleteName, L"%hs%s", pathName, findData.cFileName);

			if (!DeleteFile(deleteName))
			{
				PrintError(L"\nUnable to delete file %d", deleteName);
			}

			//	Number of files we deleted
			count ++;
		}
	} while (FindNextFile(findHandle, &findData));

	FindClose(findHandle);

	//	Output some information
	wprintf(L"\nDeleted %lld total files ", count);
	OutputSize(L"taking", count * fileIOSize);

	return true;
}


//	Output a usage message
void Usage (const char* progName)
{
	wprintf(L"\nUsage: %hs [-stats] [-create] [-verify] [-keepverifying] [-delete] <path>\n", progName);
	wprintf(L"\nExample:\n");
	wprintf(L"\n%hs -stats E:\\\n\n", progName);
}


//	Main function
int main (int argc, char** argv)
{
	if (argc < 2)
	{
		//	We need at least 2 options - output a usage message
		Usage(argv [0]);
		return 1;
	}

	//	See what the user asked for
	const char* pathName	= nullptr;
	uint8_t		progActions	= checkActions::noActions;
	for (int i = 1; i < argc; i ++)
	{
		if (strcmp(argv [i], "-stats") == 0)
		{
			//	User wants stats
			progActions |= checkActions::outputStats;
		}
		else
		if (strcmp(argv [i], "-create") == 0)
		{
			//	User wants to create files
			progActions |= checkActions::createFiles;
		}
		else
		if (strcmp(argv [i], "-verify") == 0)
		{
			//	User wants to verify files
			progActions |= checkActions::verifyFiles;
		}
		else
		if (strcmp(argv[i], "-keepverifying") == 0)
		{
			//	User wants to verify files
			progActions |= checkActions::keepVerifying;
		}
		else
		if (strcmp(argv[i], "-delete") == 0)
		{
			//	User wants to delete files
			progActions |= checkActions::deleteFiles;
		}
		else
		{
			//	Check pathname
			pathName = argv [i];

			//	Convert to wide version
			wchar_t widePath [16];
			swprintf_s(widePath, L"%hs", pathName);

			//	Get the type of drive
			auto driveType = GetDriveType(widePath);

			switch (driveType)
			{
				default:
					printf("%s is an invalid option or drive path\n", pathName);
					return 1;

				case DRIVE_REMOVABLE:
				case DRIVE_FIXED:
				case DRIVE_REMOTE:
				case DRIVE_RAMDISK:
					//	All valid
					break;
			}
		}
	}

	//	Any options?
	if (progActions == checkActions::noActions)
	{
		Usage(argv [0]);
		return 1;
	}

	//	We need to get stats for this device
	DWORD bytesPerSector;
	DWORD sectorsPerCluster;
	DWORD freeClusters;
	DWORD totalClusters;
	if (GetDiskFreeSpaceA(pathName, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters) == 0)
	{
		printf("Error: Could not get disk stats 0x%X\n", GetLastError());
		return 1;
	}

	//	Using DWORD, the free space could overflow
	uint64_t freeSpace	=	bytesPerSector;
	freeSpace			*=	sectorsPerCluster;
	freeSpace			*=	freeClusters;

	//	Same for total space
	uint64_t totalSpace	=	bytesPerSector;
	totalSpace			*=	sectorsPerCluster;
	totalSpace			*=	totalClusters;

	//	See what we need to do
	if ((progActions & checkActions::outputStats) != 0)
	{
		//	Output some stats
		printf("Bytes/sector     : %d\n", bytesPerSector);
		printf("Sectors/cluster  : %d\n", sectorsPerCluster);

		//	Get the human readable version of the total size
		OutputSize(L"Total space      : ", totalSpace);

		//	Get the human readable version of the free space
		OutputSize(L"Free space       : ", freeSpace);
	}


	//	Create files
	if ((progActions & checkActions::createFiles) != 0)
	{
		if (!CreateFiles(pathName, bytesPerSector, freeSpace))
		{
			wprintf(L"File creation failed\n");
			return 1;
		}
	}

	//	Verify files
	if ((progActions & checkActions::verifyFiles) != 0)
	{
		if (!VerifyFiles(pathName, bytesPerSector, (progActions & checkActions::keepVerifying) != 0))
		{
			wprintf(L"File verification failed\n");
			return 1;
		}
	}

	//	Delete files we created
	if ((progActions & checkActions::deleteFiles) != 0)
	{
		if (!DeleteFiles(pathName))
		{
			wprintf(L"File deletion failed\n");
			return 1;
		}
	}

	//	All done!
	return 0;
}