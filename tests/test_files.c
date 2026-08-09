// Tests: file system functions (no window needed)
#include "raylib.h"
#include "testkit.h"

#include <stdio.h>

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    const char *dir = "rayvk_test_dir";
    const char *file = "rayvk_test_dir/data.bin";
    const char *textFile = "rayvk_test_dir/text.txt";

    CHECK_EQ_INT(MakeDirectory(dir), 0);
    CHECK(DirectoryExists(dir));

    // Binary data roundtrip
    unsigned char payload[256];
    for (int i = 0; i < 256; i++) payload[i] = (unsigned char)i;
    CHECK(SaveFileData(file, payload, 256));
    CHECK(FileExists(file));
    CHECK_EQ_INT(GetFileLength(file), 256);
    int size = 0;
    unsigned char *loaded = LoadFileData(file, &size);
    CHECK_EQ_INT(size, 256);
    CHECK(memcmp(loaded, payload, 256) == 0);
    UnloadFileData(loaded);

    // Text roundtrip
    CHECK(SaveFileText(textFile, "line one\nline two"));
    char *text = LoadFileText(textFile);
    CHECK_EQ_STR(text, "line one\nline two");
    UnloadFileText(text);

    // Text find/replace in file
    CHECK(FileTextFindIndex(textFile, "two") > 0);
    CHECK_EQ_INT(FileTextFindIndex(textFile, "absent"), -1);
    CHECK_EQ_INT(FileTextReplace(textFile, "two", "2"), 0);   // returns 0 on success
    text = LoadFileText(textFile);
    CHECK_EQ_STR(text, "line one\nline 2");
    UnloadFileText(text);

    // Path helpers
    CHECK(IsFileExtension("image.PNG", ".png"));
    CHECK(!IsFileExtension("image.png", ".jpg"));
    CHECK_EQ_STR(GetFileExtension("a/b/c.tar.gz"), ".gz");
    CHECK_EQ_STR(GetFileName("/x/y/z.txt"), "z.txt");
    CHECK_EQ_STR(GetFileNameWithoutExt("/x/y/z.txt"), "z");
    CHECK(IsPathFile(file));
    CHECK(!IsPathFile(dir));
    CHECK(IsFileNameValid("normal_name.txt"));
    CHECK(!IsFileNameValid(""));
    CHECK(GetFileModTime(file) > 0);

    // Directory listing
    FilePathList list = LoadDirectoryFiles(dir);
    CHECK_EQ_INT((int)list.count, 2);
    UnloadDirectoryFiles(list);
    CHECK_EQ_INT((int)GetDirectoryFileCount(dir), 2);
    FilePathList filtered = LoadDirectoryFilesEx(dir, ".txt", false);
    CHECK_EQ_INT((int)filtered.count, 1);
    UnloadDirectoryFiles(filtered);

    // Copy/rename/move/remove
    CHECK_EQ_INT(FileCopy(file, "rayvk_test_dir/copy.bin"), 0);
    CHECK(FileExists("rayvk_test_dir/copy.bin"));
    CHECK_EQ_INT(FileRename("rayvk_test_dir/copy.bin", "rayvk_test_dir/renamed.bin"), 0);
    CHECK(FileExists("rayvk_test_dir/renamed.bin"));
    CHECK(!FileExists("rayvk_test_dir/copy.bin"));
    CHECK_EQ_INT(FileRemove("rayvk_test_dir/renamed.bin"), 0);
    CHECK(!FileExists("rayvk_test_dir/renamed.bin"));

    // Missing file behaviors
    int missingSize = -1;
    unsigned char *missing = LoadFileData("does_not_exist.bin", &missingSize);
    CHECK(missing == NULL);
    CHECK(!FileExists("does_not_exist.bin"));

    // Cleanup
    FileRemove(file);
    FileRemove(textFile);
    remove(dir);

    return tk_report("test_files");
}
