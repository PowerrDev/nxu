# Test definition: tep-mailbox
TEST_tep-mailbox_GROUP := Trusted Enclave
TEST_tep-mailbox_DESC := Mailbox to tepOS over serial1: protocol, crypto, passcode, bootd manifest, fail closed, reconnect (tepOS must be running)
TEST_tep-mailbox_CFLAGS := -DNXU_TEP_MAILBOX_TEST
TEST_tep-mailbox_SERIAL1 := $(TEP_SERIAL1)
