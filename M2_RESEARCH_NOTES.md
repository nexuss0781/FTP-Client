# M2 Research Notes

## RFC 4217 — Securing FTP with TLS

Source: https://datatracker.ietf.org/doc/html/rfc4217

The explicit FTPS sequence starts on the normal FTP control port. The client sends `AUTH TLS`; a `234` reply indicates that the server accepts TLS negotiation. After the TLS handshake, the client must re-authorize with `USER` and `PASS`. To protect data connections, `PBSZ` must precede `PROT`, and FTP-TLS uses `PBSZ 0` because the TLS layer is a streaming protection mechanism. `PROT P` requests private data-channel protection; `PROT C` would leave the data channel clear and is not the M2 default. The client must not silently downgrade when secure mode is requested.

RFC 4217 also states that FTPS servers should advertise `AUTH TLS`, `PBSZ`, and `PROT` through `FEAT` when supported. M2 will implement the control-channel negotiation and protected-session state; passive data-channel TLS remains a later milestone.

## RFC 6125 — TLS Service Identity

Source: https://datatracker.ietf.org/doc/html/rfc6125

The client must validate the complete certificate path and match the server’s presented service identity to the reference hostname. The preferred certificate identity is the `subjectAltName` `dNSName`; Common Name fallback is not the preferred modern rule. M2 will use the configured host as the reference identity and fail verification when the chain or hostname does not match.

## OpenSSL `SSL_set1_host`

Source: https://docs.openssl.org/3.6/man3/SSL_set1_host/

OpenSSL recommends configuring `SSL_set1_host()` for hostname checks and `SSL_set_tlsext_host_name()` for SNI. With a nonempty host, OpenSSL performs hostname verification through `X509_check_host()` during certificate verification. M2 will configure both SNI and the expected host before `SSL_connect()` and will rely on OpenSSL’s verification result rather than maintaining a parallel permissive hostname matcher.
