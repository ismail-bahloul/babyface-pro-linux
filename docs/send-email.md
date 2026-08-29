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

## Generate the series (threaded cover letter + patch)

From a linux-next checkout where the driver has been integrated
(`sound/usb/babyfacepro/` + Makefile/Kconfig/MAINTAINERS):

```sh
git format-patch --rfc -1 --cover-letter -o /tmp/rfcpatch/
# edit /tmp/rfcpatch/0000-cover-letter.patch:
#   replace "*** SUBJECT HERE ***" with the real subject
#   replace "*** BLURB HERE ***" with the body (see patches/COVER-LETTER.md)
```

## Send

```sh
cd /home/iswad/DATA/05_Code/Projects/babyface-pro-linux

git send-email \
  --from='Ismaïl Bahloul <i.bahloul01@gmail.com>' \
  --to=linux-sound@vger.kernel.org \
  --cc=linux-usb@vger.kernel.org \
  --cc=alsa-devel@alsa-project.org \
  --cc=perex@perex.cz \
  --cc=tiwai@suse.com \
  --cc=linux-kernel@vger.kernel.org \
  /tmp/rfcpatch/0000-cover-letter.patch \
  /tmp/rfcpatch/0001-ALSA-usb-add-RME-Babyface-Pro-FS-driver-proprietary-.patch
```

At the `Password for 'smtp.gmail.com':` prompt, paste the App Password
(spaces are OK). At the `Send this email?` prompt, `a` confirms all
emails in the series. `Result: 250` after each email means it was
accepted.

## Recipient notes

- Re-run `get_maintainer.pl` before mailing, MAINTAINERS entries change.
- `alsa-devel` is moderated for non-subscribers, so that copy may be
  delayed; `linux-sound` and `linux-kernel` go out immediately.
