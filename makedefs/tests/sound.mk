# Test definition: sound
TEST_sound_GROUP := Sound
TEST_sound_DESC := Tone, boot chime and playsound through VirtIO Sound (audible)
TEST_sound_PASS := sound_test: passed
TEST_sound_CFLAGS := -DNXU_SOUND_TEST
TEST_sound_TIMEOUT := 120
TEST_sound_CAPTURE := 1
TEST_sound_VERIFY := python3 tools/audio/verify_capture.py @CAPTURE@ tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.wav --tone --chimes 2
