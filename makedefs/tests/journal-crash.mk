# Test definition: journal-crash
TEST_journal-crash_GROUP := Filesystem
TEST_journal-crash_DESC := Crash mid-transaction; make run replays it
TEST_journal-crash_CFLAGS := -DNXU_JOURNAL_CRASH_TEST
TEST_journal-crash_DISPLAY := cocoa
