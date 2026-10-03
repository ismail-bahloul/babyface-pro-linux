# Sending patches with git send-email

How to mail a kernel patch series from this repo with `git send-email`
(via Gmail SMTP). Written down so a future submission (v2, etc.) is a
paste, not a re-investigation.

## Prerequisites (CachyOS/Arch)

```sh
sudo pacman -S perl-io-socket-ssl perl-authen-sasl
```

Without these Perl modules `git send-email` can't do TLS with
certificate verification (`IO::Socket::SSL`) or SMTP auth (`Authen::SASL`),
and it bails out at the SMTP step.

You also need a **Gmail App Password** (the normal password won't work
for SMTP): Google Account → Security → 2-Step Verification → App
passwords → create one (name it `git`). Store it in your password
manager, not in this file.

## Repo-local SMTP config (already set in this repo)

```
git config sendemail.smtpserver smtp.gmail.com
git config sendemail.smtpserverport 587
git config sendemail.smtpencryption tls
git config sendemail.smtpuser i.bahloul01@gmail.com
git config sendemail.from "Ismaïl Bahloul <i.bahloul01@gmail.com>"
git config sendemail.confirm auto
```

## Generate the series (threaded cover letter + 3 patches)

From the linux-next checkout where the driver is integrated
(`sound/usb/babyfacepro/` + Makefile/Kconfig/MAINTAINERS), on the
branch holding the series:

```sh
git format-patch --rfc --cover-letter -v4 -o /tmp/v4 origin/master..v4b
```

**Two traps that bit v4 while it was being cut:**

1. The linux-next clone's local git config still says
   `Iswad <iswadlillah@gmail.com>`. The commits are fine (they were
   made with `-c user.name/-c user.email`), but `format-patch` stamps
   the **cover letter** with the config identity, so it came out under
   the old name while every patch said the right one. Either fix the
   clone's local config or pass `--from=`.
2. Do not put a line of dashes in a commit message. A
   `----------------` underline under a section heading looks like the
   `---` separator to `git am`, which truncates the message there.
   checkpatch catches it ("Invalid commit separator"), so always run
   checkpatch on the **generated patch files**, not only on the
   sources.

## Before sending: the blocker

Patch 1 carries `FILL-IN-BEFORE-SENDING` in place of David Fredman's
address, in both the `Co-developed-by:` and his `Signed-off-by:`. His
GitHub author address is a `users.noreply.github.com` one, which is
not valid for the DCO. The placeholder is deliberate: checkpatch
reports it as an ERROR and `git send-email` cannot parse it as an
address, so the series cannot go out by accident with his credit
missing or wrong. He has been asked for the name and address he wants
(issue #4).

## Send

The current series is v6 (`patches/v6-*.patch`).  Run it once with
`--dry-run` first: it prints every mail with its recipients and sends
nothing.

```sh
cd /home/iswad/DATA/05_Code/Projects/babyface-pro-linux

git send-email \
  --from='Ismaïl Bahloul <i.bahloul01@gmail.com>' \
  --to=linux-sound@vger.kernel.org \
  --cc=tiwai@suse.com \
  --cc=perex@perex.cz \
  --cc=linux-usb@vger.kernel.org \
  --cc=alsa-devel@alsa-project.org \
  --cc=linux-kernel@vger.kernel.org \
  --cc=corbet@lwn.net \
  --cc=skhan@linuxfoundation.org \
  --cc=rdunlap@infradead.org \
  --cc=linux-doc@vger.kernel.org \
  patches/v6-00*.patch
```

The last four Cc lines are the Documentation reviewers `get_maintainer.pl`
names for patch 8.  `git send-email` adds David Fredman by itself, from
his `Co-developed-by`/`Signed-off-by` trailers (patches 1, 2, 3, 5, 6).
`get_maintainer.pl` also lists "Mark Brown": that is the linux-next
"Add linux-next specific files" commit, not a contributor to this
driver, so he is left out.

At the `Password for 'smtp.gmail.com':` prompt, paste the App Password
(spaces are OK). At the `Send this email?` prompt, `a` confirms all
emails in the series. `Result: 250` after each email means accepted.

## Recipient notes

- Re-run `get_maintainer.pl` before mailing, MAINTAINERS entries change.
- It is clean as of 2026-09-13: Jaroslav Kysela, Takashi Iwai and the
  sound lists. It was **not** clean before that date: two comments
  mentioned the userspace app's Rust implementation, and
  `get_maintainer.pl` matches `\brust\b`, so it pulled the entire Rust
  for Linux review team and rust-for-linux@vger.kernel.org onto a USB
  audio patch. The word is gone from the sources; keep it that way.
- `alsa-devel` is moderated for non-subscribers, so that copy may be
  delayed; `linux-sound` and `linux-kernel` go out immediately.
