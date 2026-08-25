# Licensing FAQ (for procurement)

This is not legal advice. It restates what the in-tree licence files already say.
The texts of record are [LICENSE-AGPL](LICENSE-AGPL) and
[LICENSE-COMMERCIAL](LICENSE-COMMERCIAL). [NOTICE](NOTICE) lists third-party
components. Qt LGPL evidence is in [docs/LGPL_QT.md](docs/LGPL_QT.md).

## Can we run this air-gapped without disclosing our source?

**Yes, under the commercial licence.** [LICENSE-COMMERCIAL](LICENSE-COMMERCIAL)
allows you to use RetDec in commercial and internal products, keep your changes
private, ship binaries of your product, and run it as a hosted or on-prem
service. There is no network-copyleft clause in that text.

The commercial licence has no numbered clauses. Those permissions are the
bullets under **What you can do**.

Air-gapped operation of the decompiler itself does not require a model download.
Optional neural refinement is local llama.cpp only; see [SECURITY.md](SECURITY.md)
(`RETDEC_NO_NETWORK=1`).

## What if we use AGPL instead?

[LICENSE-AGPL](LICENSE-AGPL) is GNU AGPL 3.0 or later. If you modify the
program and run it as a network service, AGPL section 13 can require you to
offer corresponding source to users who interact with it remotely. That is the
procurement concern the commercial licence is meant to avoid.

Students, researchers, and anyone happy to share source can use AGPL without
buying a commercial licence. [LICENSE-COMMERCIAL](LICENSE-COMMERCIAL) says so
under **Who it is for**.

## Can we embed RetDec in our own product?

- **AGPL:** yes, if you meet AGPL corresponding-source obligations for the
  combination.
- **Commercial:** yes. You may keep your changes private and ship binaries.
  You must not claim you own RetDec or the Imortek name, and you must not sell
  RetDec itself as if it were your original work (**What you cannot do**).

After payment, keep a discreet “Powered by RetDec (Imortek)” line in docs or an
About box (**Day one**).

## How do we buy?

Email **odin.loch@outlook.com.au** with company name and country, intended use
(internal, SaaS, OEM / redistribution), and seat count, as listed under
**How to buy** in [LICENSE-COMMERCIAL](LICENSE-COMMERCIAL). Prices are not
published in git.

## Which licence governs Keystone and Qt?

Keystone is GPL-2.0 and is excluded from commercial-package install of
`capstone2llvmirtool` (see [NOTICE](NOTICE) and the LEG-11 CI gate). Qt 6 is
LGPL and must stay dynamically linked; see [docs/LGPL_QT.md](docs/LGPL_QT.md).

## Warranty

The commercial text provides the software as-is, with liability limited to fees
paid in the previous twelve months except where Australian consumer law says
otherwise. Governing law is Australia.
