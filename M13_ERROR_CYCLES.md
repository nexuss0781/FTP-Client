# M13 Error-Cycle Recap

**Project:** FTP-Client  
**Milestone:** M13  
**Branch:** `feature/ftps-m13-verification-retry-policies`  
**Author:** Manus AI

## Cycle 1: Verification policy was introduced below its first use

The first M13 policy implementation called the existing feature-token normalization helper from the inline download method before the helper’s later definition in `ProtocolEngine.hpp`. The compiler correctly rejected the unresolved name.

The resolution was a forward declaration placed immediately before the inline download implementation. The existing helper remains the single normalization implementation, avoiding duplicate token logic.

## Cycle 2: LOCAL_AND_REMOTE was initially over-constrained

The first policy validation required an expected digest for both `LOCAL_EXPECTED` and `LOCAL_AND_REMOTE`. That was incorrect: `LOCAL_AND_REMOTE` is specifically useful when the client computes the local digest and compares it directly with the server HASH response without a separately supplied manifest digest.

The resolution was to require an expected digest only for `LOCAL_EXPECTED`. `LOCAL_AND_REMOTE` now computes the local digest, retrieves the remote digest, and compares the two while still accepting an optional expected digest when one is present.

## Cycle 3: M13 retry wrapping changed M8 stall behavior

After retrying downloads through the richer `RetryPolicy`, the M8 stalled-transfer test no longer observed the expected immediate `FTP_ERR_STALLED` outcome. The existing classifier treated unknown negative codes as transient, and `FTP_ERR_STALLED` had not been listed among the permanent non-retryable cases.

The resolution was to classify `FTP_ERR_STALLED` alongside cancellation, invalid state, and integrity failures as a permanent local category. Default policy behavior is therefore backward compatible, while explicit retry-all remains available for callers that intentionally want broader retries.

## Cycle 4: Single-file and directory downloads needed the same retry surface

M12’s directory scheduler already had a TransferConfig path, but the single-file C ABI download called ProtocolEngine directly. Exposing M13 fields only on directory downloads would have created inconsistent policy behavior.

The resolution was to wrap single-file C ABI downloads in the same RetryPolicy configuration and to map the same fields into directory TransferConfig. Upload tasks use the same fields through the existing TransferEngine retry wrapper.

## Cycle 5: Tail-extension compatibility required guarded decoding

M13 added fields to both upload and download option structures. Reading them unconditionally would break callers compiled against an older header or callers passing a smaller struct.

The resolution was to gate every new read with `offsetof(field) + sizeof(field) <= struct_size`. Zero and omitted fields retain the established defaults; nonzero policy values are applied only when their field is covered by the caller’s declared structure size.

## Cycle 6: ABI and Python surfaces had to stay synchronized

The native header, ctypes regression structures, cffi-generated declarations, Python dataclasses, option construction, constants, and type stubs all needed the same M13 field order. The final ABI suite checks policy and retry attributes, while Python tests cover values, defaults, and invalid policy/jitter validation.

## Validation resolution

The final clean Debug build passed all 15 CTest cases. The Python ABI suite passed **58/58**, the exception suite passed **4/4**, and the type suite passed **8/8**. A separate clean ASan/UBSan build passed all 15 CTest cases with no sanitizer diagnostics.
