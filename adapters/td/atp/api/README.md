# Shenzhen ATP TD SDK

The Shenzhen TD adapter uses the broker-provided ATP Quant API. The SDK headers
and `libatpquantapi.so` are deployment dependencies and are intentionally
excluded from Git because the vendor license prohibits redistribution. Place
the vendor SDK under `api/include/` and `api/lib/`, or set `SZE_TD_API_DIR` to
an external directory containing `include/` and `lib/`, before building
`libsze_td.so`.

The adapter is compiled from the same `TDEngineGXBSE` implementation used by
the existing Beijing deployment, with these Shenzhen-specific definitions:

- source id: `180`
- engine key: `sze_td`
- default ATP market id: `102` (Shenzhen)

Credentials and endpoint settings must be supplied through a root-only runtime
configuration. They are not stored in this repository.
