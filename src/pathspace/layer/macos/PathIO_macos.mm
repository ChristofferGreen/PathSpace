/*
 PathSpace: macOS PathIO compatibility translation unit (.mm)

 This file is intentionally empty.

 Rationale:
 - macOS-specific classes PathIOMiceMacOS and PathIOKeyboardMacOS have been removed.
 - Unified providers PathIOMice and PathIOKeyboard now handle platform selection via compile-time macros.
 - This TU remains only to satisfy build scripts that may still reference it.

 No symbols are defined here.
*/
