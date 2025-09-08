//	Check how much disk space a device actually has. This is
//	done by creating a large file and then writing and reading
//	specific patterns to verify data made it to the device
//
//	License: MIT. See the LICENSE file in the project root for more details.
//

#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

#include <chrono>

//	Size metrics e.g. KiB, GiB etc.
constexpr int64_t KiB = 1024;
constexpr int64_t MiB = KiB * 1024;
constexpr int64_t GiB = MiB * 1024;
constexpr int64_t TiB = GiB * 1024;

//	Converts bytes to human readable sizes
constexpr int64_t			sizeArray []	= { TiB, GiB, MiB, KiB};
constexpr const wchar_t*	sizeNames []	= { L"TiB", L"GiB", L"MiB", L"KiB"};
constexpr const wchar_t*	sizeIsBytes		= L"bytes";
constexpr int				numSizes		= sizeof(sizeArray) / sizeof(sizeArray[0]);

//	File prefix
constexpr const char*		verifyFilename	= "verifysp.bin";

//	Verification block size
constexpr uint64_t			verifySize		= 10 * MiB;

//	Batch size for some operations
constexpr uint64_t			batchSize		= 5;

//	Program actions
namespace progActions
{
	uint8_t justPath	= 1;
	uint8_t cached		= 2;
	uint8_t noreads		= 4;
	uint8_t outputStats = 8;
};


//	Output an error message
void PrintError (const wchar_t * format, ...)
{
	//	We start by saving the current error as we might make
	//	API calls that produce other errors
	auto savedError = GetLastError();

	//	There are two parts to the error message. There's the Windows
	//	description of the error and information passed by the user.
	//	Start by getting the Windows error text
	LPCTSTR windowsMsg = nullptr;

	//	Format the error message
	FormatMessage(	FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					nullptr, savedError,
					MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
					(LPTSTR) &windowsMsg, 0, nullptr);

	//	User message
	wchar_t userMsg [BUFSIZ];
	
	//	Get the start of the variable arguments
	va_list ourArgs;
	va_start(ourArgs, format);
	vswprintf_s(userMsg, format, ourArgs);

	//	Output the full message
	wprintf(L"%s : %s\n", userMsg, windowsMsg);

	//	Free off the Windows message buffer
	LocalFree((LPVOID) windowsMsg);
}


//	Output a human readable size
const wchar_t* HumanReadable (int64_t sizeInBytes, int64_t& convertedSize)
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
	return sizeIsBytes;
}


//	Common output function for sizes
void OutputSize (const wchar_t* msg, const uint64_t inSize)
{
	int64_t converted;
	const wchar_t* textSize = HumanReadable(inSize, converted);
	wprintf(L"%s %lld %s\n", msg, converted, textSize);
}


//	We need to obtain a certain privelege to manipulate
//	the verification file the way we want
bool AddPrivelege (LPCTSTR privName)
{
	HANDLE tokenHandle = INVALID_HANDLE_VALUE;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle)) 
	{
		PrintError(L"Could not get token handle for %s", privName);
		return false;
	}

	//	Lookup the privelege
	LUID lookupID;
	LookupPrivilegeValue(NULL, privName, &lookupID);

	//	Add the new privelege
	TOKEN_PRIVILEGES newPriv;
	ZeroMemory(&newPriv, sizeof(newPriv));
	newPriv.PrivilegeCount				= 1;
	newPriv.Privileges[0].Luid			= lookupID;
	newPriv.Privileges[0].Attributes	= SE_PRIVILEGE_ENABLED;

	DWORD				returnLen;
	TOKEN_PRIVILEGES	oldPriv;
	if (!AdjustTokenPrivileges(tokenHandle, FALSE, &newPriv, sizeof(TOKEN_PRIVILEGES), &oldPriv, &returnLen))
	{
		CloseHandle(tokenHandle);
		PrintError(L"Unable to get privilege %s", privName);
		return false;
	}

	if (!CloseHandle(tokenHandle))
	{
		//	Could not close the token handle
		PrintError(L"Could not close the handle for %s", privName);
		return false;
	}

	return true;
}


//	Quickly create the file
bool CreateVerifyFile (const char* pathName, const DWORD bytesPerSector, const int64_t totalSpace)
{
	//	We need to get the SE_MANAGE_VOLUME_NAME privelege to allow
	//	us to manipulate the verification file correctly.
	//
	//	Getting this privilege allows us to move the file pointer 
	//	around without Windows writing zeroes to the file
	if (!AddPrivelege(SE_MANAGE_VOLUME_NAME))
	{
		return false;
	}

	//	Create the filename
	wchar_t writeName [MAX_PATH];
	swprintf_s(writeName, L"%hs%hs", pathName, verifyFilename);

	//	Output some information
	wprintf(L"Creating file %s", writeName);
	OutputSize(L", will be", totalSpace);

	//	Create the file
	HANDLE writeFile = CreateFile(writeName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (writeFile == INVALID_HANDLE_VALUE)
	{
		PrintError(L"Could not create %s", writeName);
		return false;
	}

	//	The file is now open for writing. We are going to move the file 
	//	pointer to the value of totalSpace, which could be several GiB 
	//	or TiB. We then set the valid data offset and EOF.
	//
	//	This allows us to create a very large file, very quickly
	//
	LARGE_INTEGER fileOffset;
	fileOffset.QuadPart = totalSpace;
	if (!SetFilePointerEx(writeFile, fileOffset, nullptr, FILE_BEGIN))
	{
		PrintError(L"Could not set file pointer on %s", writeName);
		CloseHandle(writeFile);
		return false;
	}

	//	File pointer is set, mark the new EOF
	if (!SetEndOfFile(writeFile))
	{
		PrintError(L"Could not set end of file on %s", writeName);
		CloseHandle(writeFile);
		return false;
	}

	//	If we don't set the valid data pointer, Windows will
	//	attempt to write zeroes into the entire file, which
	//	we don't want
	if (!SetFileValidData(writeFile, fileOffset.QuadPart))
	{
		PrintError(L"Could not set valid data size on %s", writeName);
		CloseHandle(writeFile);
		return false;
	}

	//	We can close the file
	if (!CloseHandle(writeFile))
	{
		PrintError(L"Could not close file %s after creation", writeFile);
		return false;
	}

	return true;
}


//	Common clean up code
inline void CommonVerifyCleanup (HANDLE verifyFile, uint8_t* verifyBuffer)
{
	if (verifyFile)
	{
		if (!CloseHandle(verifyFile))
		{
			PrintError(L"Could not close verification file");
		}
	}

	if (verifyBuffer != nullptr)
	{
		_aligned_free(verifyBuffer);
	}
}


//	Verify the created file is the correct size
bool VerifyTheFile (const char* pathName, const DWORD bytesPerSector, const bool noReads, const bool cached)
{
	//	Create the verification filename
	wchar_t verifyName [MAX_PATH];
	swprintf_s(verifyName, L"%hs%hs", pathName, verifyFilename);

	//	Set default values
	HANDLE		verifyFile		= INVALID_HANDLE_VALUE;
	uint8_t*	verifyBuffer	= nullptr;

	//	See what type of caching we were asked to use
	DWORD fileAttributes;
	if (cached)
	{
		//	File system cache allowed
		fileAttributes = FILE_ATTRIBUTE_NORMAL;
	}
	else
	{
		//	File system cache is not allowed
		fileAttributes = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
	}

	//	Open the file
	verifyFile = CreateFile(verifyName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, fileAttributes, nullptr);
	if (verifyName == INVALID_HANDLE_VALUE)
	{
		PrintError(L"Could not open %s for verification", verifyName);
		return false;
	}

	//	We need to know how big the file is
	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(verifyFile, &fileSize))
	{
		PrintError(L"Could not get the file size for %s", verifyName);
		CommonVerifyCleanup(verifyFile, verifyBuffer);
		return false;
	}

	//	Output some information
	uint64_t totalBlocks = fileSize.QuadPart / verifySize;
	wprintf(L"Verification of %s will use %lld blocks of", verifyName, totalBlocks);
	OutputSize(L"", verifySize);

	//	Create a buffer that we can use to verify markers
	verifyBuffer = (uint8_t*) _aligned_malloc(bytesPerSector, bytesPerSector);
	if (verifyBuffer == nullptr)
	{
		PrintError(L"Did not get verify buffer for %s", verifyName);
		CommonVerifyCleanup(verifyFile, verifyBuffer);
		return false;
	}

	//	Start the timer
	auto start		= std::chrono::high_resolution_clock::now();
	auto elapsed	= std::chrono::high_resolution_clock::now();

	//	Write and then read the verification markers at certain points in the file
	uint64_t count = 0;
	for (LONGLONG i = 0; i < fileSize.QuadPart; i += verifySize)
	{
		//	Output some stats if it is time
		if (count && count % batchSize == 0)
		{
			//	Get the end time
			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> blockSeconds		= end - start;
			std::chrono::duration<double> elapsedSeconds	= end - elapsed;

			//	Let the user know how long these blocks took
			wprintf(L"\rProcess verification block %lld/%lld took %.2lf seconds (%.2lf total seconds)   ", count, totalBlocks, blockSeconds.count(), elapsedSeconds.count());
			start = std::chrono::high_resolution_clock::now();
		}

		//	Move to that part of the file
		LARGE_INTEGER fileOffset;
		fileOffset.QuadPart = i;
		if (!SetFilePointerEx(verifyFile, fileOffset, nullptr, FILE_BEGIN))
		{
			PrintError(L"\nUnable to move verification file pointer for %s", verifyName);
			OutputSize(L"Reached", i);
			CommonVerifyCleanup(verifyFile, verifyBuffer);
			return false;
		}

		//	Clear the verification buffer
		memset(verifyBuffer, 0, bytesPerSector);

		//	Set verification data
		//
		//	This will be the current count + 1 at multiple offsets 
		//	in the buffer
		const uint64_t dataOffsets = bytesPerSector / 4;
		for (int o = 0; o < 4; o++)
		{
			uint64_t* dataPtr = (uint64_t*) (verifyBuffer + (o * dataOffsets));
			*dataPtr = count + 1;
		}

		//	Write the data
		DWORD written;
		if (WriteFile(verifyFile, verifyBuffer, bytesPerSector, &written, nullptr) == 0)
		{
			PrintError(L"\nCould not write to %s", verifyName);
			OutputSize(L"Reached", i);
			CommonVerifyCleanup(verifyFile, verifyBuffer);
			return false;
		}

		//	Sanity check
		if (written != bytesPerSector)
		{
			//	Give a clear indication where the write error was
			wprintf(L"\n%s wrote %d bytes, expected %d bytes @ offset %lld", 
						verifyName, written, bytesPerSector, i);
			OutputSize(L" ", i);

			//	Clean up and bail
			CommonVerifyCleanup(verifyFile, verifyBuffer);
			return false;
		}

		if (!noReads)
		{
			//	Need to set the file pointer back a block
			if (!SetFilePointerEx(verifyFile, fileOffset, nullptr, FILE_BEGIN))
			{
				PrintError(L"\nMove read file pointer");
				OutputSize(L"Reached", i);
				CommonVerifyCleanup(verifyFile, verifyBuffer);
				return false;
			}

			//	Reset the buffer pattern to something very different than before
			memset(verifyBuffer, 0xFF, bytesPerSector);

			//	Read the data
			DWORD bytesRead;
			if (ReadFile(verifyFile, verifyBuffer, bytesPerSector, &bytesRead, nullptr) == 0)
			{
				PrintError(L"\nUnable to read from %s", verifyFile);
				OutputSize(L"Reached", i);
				CommonVerifyCleanup(verifyFile, verifyBuffer);
				return false;
			}

			//	Sanity check
			if (bytesRead != bytesPerSector)
			{
				//	Give a clear indication where the read error was
				wprintf(L"\n%s read %d bytes, expected %d bytes @ offset %lld",
					verifyName, bytesRead, bytesPerSector, i);
				OutputSize(L"", i);

				//	Clean up and bail
				CommonVerifyCleanup(verifyFile, verifyBuffer);
				return false;
			}

			//	Read unique data from the buffer
			for (int o = 0; o < 4; o++)
			{
				uint64_t* dataPtr = (uint64_t*)(verifyBuffer + (o * dataOffsets));
				if (*dataPtr != count + 1)
				{
					//	Give the user an idea of where the verification failed
					wprintf(L"\nVerification data %lld is incorrect should be %lld @ offset %lld", *dataPtr, count + 1, i);
					OutputSize(L"", i);

					//	Clean up and bail
					CommonVerifyCleanup(verifyFile, verifyBuffer);
					return false;
				}
			}
		}

		//	Next block
		count ++;
	}

	//	Tell the user the good news
	wprintf(L"\n%hs ", pathName);
	OutputSize(L"is", fileSize.QuadPart);

	//	All done
	CommonVerifyCleanup(verifyFile, verifyBuffer);
	return true;
}


//	Delete the file we created
bool DeleteVerifyFile (const char* pathName)
{
	//	Create the search path
	wchar_t deleteFile[MAX_PATH];
	swprintf_s(deleteFile, L"%hs%hs", pathName, verifyFilename);

	//	Output some information
	wprintf(L"Removing file %s\n", deleteFile);

	if (!DeleteFile(deleteFile))
	{
		PrintError(L"Unable to delete file %s", deleteFile);
		return false;
	}

	return true;
}


//	Output a usage message
void Usage (const char* progName)
{
	wprintf(L"\nUsage: %hs [-stats] [-noreads] [-cached] <path>\n", progName);
	wprintf(L"\nExample:\n");
	wprintf(L"\n%hs -stats E:\\\n\n", progName);
}


//	Main function
int main (int argc, char** argv)
{
	if (argc < 2)
	{
		//	We need at least 2 options - output a usage message
		Usage(argv[0]);
		return 1;
	}

	//	See what the user asked for
	const char* pathName = nullptr;
	uint8_t		ourActions = progActions::justPath;
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv [i], "-stats") == 0)
		{
			//	User wants stats
			ourActions |= progActions::outputStats;
		}
		else
		if (strcmp(argv [i], "-cached") == 0)
		{
			//	User wants to use file system cache
			ourActions |= progActions::cached;
		}
		else
		if (strcmp(argv[i], "-noreads") == 0)
		{
			//	User only wants to write to the device
			ourActions |= progActions::noreads;
		}
		else
		{
			//	Check pathname
			pathName = argv [i];

			//	Convert to wide version
			wchar_t widePath[16];
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

	//	We need to get stats for this device
	DWORD bytesPerSector;
	DWORD sectorsPerCluster;
	DWORD freeClusters;
	DWORD totalClusters;
	if (GetDiskFreeSpaceA(pathName, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters) == 0)
	{
		PrintError(L"Could not get disk stats for %hs", pathName);
		return 1;
	}

	//	Using DWORD, the free space could overflow
	int64_t freeSpace	=	bytesPerSector;
	freeSpace			*=	sectorsPerCluster;
	freeSpace			*=	freeClusters;

	//	Same for total space
	int64_t totalSpace	=	bytesPerSector;
	totalSpace			*=	sectorsPerCluster;
	totalSpace			*=	totalClusters;

	//	Sanity check - we use file offsets later which are signed
	if (freeSpace	<= 0
	||	totalSpace	<= 0)
	{
		wprintf(L"Incorrect total %lld or free space %lld\n", totalSpace, freeSpace);
		return 1;
	}

	//	See what we need to do
	if ((ourActions & progActions::outputStats) != 0)
	{
		//	Output some stats
		wprintf(L"Bytes/sector     : %d\n", bytesPerSector);
		wprintf(L"Sectors/cluster  : %d\n", sectorsPerCluster);

		//	Get the human readable version of the total size
		OutputSize(L"Total space      : %lld %s\n", totalSpace);

		//	Get the human readable version of the free space
		OutputSize(L"Free space       : %lld %s\n", freeSpace);
	}


	//	Create the file and add markers
	if (!CreateVerifyFile(pathName, bytesPerSector, freeSpace))
	{
		wprintf(L"File creation failed\n");
		return 1;
	}

	//	Verify the markers in the file
	int returnStatus = 0;
	if (!VerifyTheFile(pathName, bytesPerSector, (ourActions & progActions::noreads) != 0, (ourActions & progActions::cached) != 0))
	{
		wprintf(L"File verification failed\n");
		returnStatus = 1;
	}

	//	Delete the file
	if (!DeleteVerifyFile(pathName))
	{
		wprintf(L"File deletion failed\n");
		returnStatus = 1;
	}

	//	All done!
	return returnStatus;

}
