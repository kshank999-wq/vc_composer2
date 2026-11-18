# Sign-in emails (Supabase Auth)

Supabase sends the sign-in, password-reset and sign-up emails. These are the
branded versions, in the same look as the site's own emails
(`src/lib/email-templates.ts`). They are pasted into the dashboard, not
deployed from here:

Supabase → Authentication → Emails → Templates
(https://supabase.com/dashboard/project/nionythbfoeqpkrnqwuf/auth/templates)

| Template | Subject | Message body |
| --- | --- | --- |
| Magic link | `Your VC Composer sign-in link` | `magic-link.html` |
| Reset password | `Choose a new VC Composer password` | `reset-password.html` |
| Confirm signup | `Confirm your email for VC Composer` | `confirm-signup.html` |

`{{ .ConfirmationURL }}` is Supabase's placeholder for the link, and `{{ .Token }}` the
one-time code the desktop app signs in with (Help > Account and Licence > Email
Me a Code); keep both. Without `{{ .Token }}` the email has no code to type.

For them to come from `VC Composer <noreply@vc-composer.com>` instead of
Supabase, Authentication → Emails → SMTP Settings uses Resend:
host `smtp.resend.com`, port `465`, username `resend`, password a Resend API
key. Authentication → URL Configuration: Site URL `https://vc-composer.com`,
Redirect URLs `https://vc-composer.com/**` and `https://www.vc-composer.com/**`.
