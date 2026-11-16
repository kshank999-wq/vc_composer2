# Deploying vc-composer.com

The site is a Next.js 14 app on Vercel, backed by a Supabase project, Stripe
(the account shared with the other VC Software products), Resend and
Cloudflare Turnstile. Do the steps in this order: each one produces a value a
later one needs. Every variable named below is listed in `web/.env.example`.

## 1. Supabase

1. Create a project (a new one, not VC Writer's: accounts, licences and
   releases are per product). Note **Project URL**, the **anon** key and the
   **service_role** key (Project Settings > API).
2. Apply the migrations in `web/supabase/migrations/`, in order:

   | File | What it makes |
   | --- | --- |
   | `0001_profiles_and_auth.sql` | `profiles`, the signup trigger, the password-set trigger, `is_admin()` |
   | `0002_commerce_and_releases.sql` | `orders`, `licenses`, `release_builds` (channels stable, preview, beta, internal), `device_activations`, `stripe_webhook_events`, `email_events`, and their row level security |
   | `0003_release_storage.sql` | the private `releases` Storage bucket |
   | `0004_rate_limits.sql` | `rate_limits` and `consume_rate_limit()` |

   With the Supabase CLI: `supabase link --project-ref <ref>` then
   `supabase db push` from `web/`. Or paste each file into the SQL editor in
   order.
3. Check **Storage**: the `releases` bucket exists and is **private**. Raise
   the global file size limit (Storage > Settings) above your largest
   installer.
4. **Authentication > URL Configuration**: Site URL `https://vc-composer.com`;
   add `https://vc-composer.com/auth/callback` (and any preview domain's
   `/auth/callback`) to the redirect allow-list.
5. Optional but recommended: Authentication > Providers > Email, switch off
   "Allow new users to sign up". The site still creates accounts itself,
   after its CAPTCHA, through the service role (`/api/auth/email-link`), and
   Stripe checkout creates them for buyers.
6. Run Supabase's advisors (Database > Advisors) and fix anything they raise.

## 2. Stripe

The account is shared with VC Writer and other products. This site acts
**only** on objects stamped `metadata.product = vc-composer` and ignores the
rest, so nothing here touches another product's sales.

1. **Product**: create "VC Composer 2" with metadata `product` = `vc-composer`.
2. **Prices**: two recurring prices on it, one monthly and one yearly. Copy
   their ids into `STRIPE_PRICE_ID_MONTHLY` and `STRIPE_PRICE_ID_YEARLY`. The
   site reads the amounts live from Stripe; there are no prices in the code.
3. **Stripe Tax**: on, with an origin address. Checkout collects a billing
   address so renewals can be taxed.
4. **Webhook endpoint**: `https://vc-composer.com/api/stripe/webhook`,
   subscribed to exactly these six events:
   - `checkout.session.completed`
   - `customer.subscription.created`
   - `customer.subscription.updated`
   - `customer.subscription.deleted`
   - `charge.refunded`
   - `charge.dispute.created`

   Copy its signing secret into `STRIPE_WEBHOOK_SECRET`. (A separate endpoint
   per product is expected: each product's webhook ignores the others' events.)
5. **Customer Portal** (Settings > Billing > Customer portal): enable it, allow
   updating payment methods, cancelling, viewing invoices, and switching
   between the monthly and yearly VC Composer 2 prices. Set the default return
   URL to `https://vc-composer.com/account`. Note the portal configuration is
   account-wide: list only VC Composer 2's prices under "products" for plan
   switching if other products share it, or create a dedicated configuration.
6. `STRIPE_SECRET_KEY`: the account's secret key (or a restricted key with
   write access to Checkout Sessions, Customers, Billing Portal, Coupons and
   Promotion Codes, and read access to Prices, Subscriptions, Invoices and
   Charges).
7. **Discount codes** are made from the site's admin console
   (`/admin/discounts`), which stamps them `product = vc-composer`. A code made
   in the Stripe dashboard works on this site only if you add that metadata to
   the promotion code or its coupon by hand. Coupons are `duration: once`, so a
   code discounts the first invoice only.

Use test-mode keys and a test webhook first, then repeat with live keys.

## 3. Resend

1. Add and verify the sending domain (e.g. `vc-composer.com`: SPF, DKIM and
   the return-path records Resend shows).
2. Create an API key with sending access: `RESEND_API_KEY`.
3. `RESEND_FROM_ADDRESS`, e.g. `VC Composer <noreply@vc-composer.com>`.

Every send is logged in `email_events` as `template@version`; a failed send
never fails the purchase (the licence is on the account page regardless).

## 4. Cloudflare Turnstile

Create a widget for `vc-composer.com` (Cloudflare dashboard > Turnstile).
`NEXT_PUBLIC_TURNSTILE_SITE_KEY` is the site key and `TURNSTILE_SECRET_KEY` the
secret. Until the secret is set the check is skipped (the rate limit still
applies), so this can come last.

## 5. Vercel

1. **Import** the GitHub repository. Set **Root Directory** to `web`. The
   framework (Next.js) is detected and also pinned by `web/vercel.json`;
   install, build and output settings stay on their defaults.
2. Node.js version: 20.x (Project Settings > General).
3. **Environment variables** (Production, and Preview with test-mode values):
   `NEXT_PUBLIC_SITE_URL`, `NEXT_PUBLIC_SUPABASE_URL`,
   `NEXT_PUBLIC_SUPABASE_ANON_KEY`, `SUPABASE_SERVICE_ROLE_KEY`,
   `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRET`, `STRIPE_PRICE_ID_MONTHLY`,
   `STRIPE_PRICE_ID_YEARLY`, `RESEND_API_KEY`, `RESEND_FROM_ADDRESS`,
   `NEXT_PUBLIC_TURNSTILE_SITE_KEY`, `TURNSTILE_SECRET_KEY`, `RATE_LIMIT_SALT`
   (any long random string), and optionally `RELEASE_BUCKET` (default
   `releases`) and `RELEASE_DOWNLOAD_TTL_SECONDS` (default `900`).
   `NEXT_PUBLIC_*` values are inlined at build time: redeploy after changing one.
4. **Domains**: add `vc-composer.com` (and `www.` redirecting to it), then point
   DNS as Vercel shows.
5. Deploy, then sign in once on the live site so your account exists.

The build needs no environment variables, so CI and a first deploy succeed
before any key exists; features fail individually at the point of use until
their keys are set.

## 6. Make yourself an administrator

After your account exists (sign in once), in the Supabase SQL editor:

```sql
update public.profiles set is_admin = true where lower(email) = lower('you@example.com');
```

Reload the site: an **Admin** link appears in the header. Revoke with
`set is_admin = false`. Nobody can set this flag from the browser.

## 7. Publish a preview build

From a terminal (or CI) with the installer built:

```sh
cd web
export SUPABASE_URL=https://<project-ref>.supabase.co
export SUPABASE_SERVICE_ROLE_KEY=...   # never commit; prefer a CI secret
node scripts/publish-release.mjs --platform windows --channel preview \
  --version 0.1.0 --min-os 10.0.17763 "path/to/VC Composer 2 Setup.exe"
node scripts/publish-release.mjs --platform macos --channel preview \
  --version 0.1.0 --min-os 11.0 "path/to/VC Composer 2.dmg"
```

Add `--dry-run` to see the key, size and SHA-256 without touching anything.
The script uploads to `releases/<platform>/<version>/<file>` and records an
**inactive** row; it refuses a version already live on that channel.

Then, at `/admin/releases`: download the waiting build, install and check it,
and press **Make live** under "Serving now: Free preview". It is now offered on
`/download#preview` and the account page to every signed-in account, and
`GET /api/releases?channel=preview` lists it. Making a newer preview live
retires the previous one on that platform automatically.

Builds can also be uploaded from `/admin/releases` directly (choose the
channel there). Stable builds follow the same steps with `--channel stable`,
and are offered only to licence holders.

The preview's own end date (after which it stops saving) is built into the
desktop binary, not set here.

## 8. Check the money path in test mode

1. `/download`: pick a plan, check out with Stripe's test card `4242 4242 4242 4242`.
2. The purchase page shows the serial (`VCC-…`); the email arrives.
3. `/account` shows the licence, stable downloads, Manage subscription.
4. Cancel in the portal: the licence keeps working to the period end, then
   `GET /api/licenses/standing` reports it ended.
5. Refund the payment in Stripe: the order turns `refunded` and the licence
   `revoked`.
6. `/admin` dashboard shows the order; `stripe_webhook_events` has every event
   with `processed_at` set.

## The live setup (November 16, 2026)

- **Supabase:** project `vc-composer` (ref `nionythbfoeqpkrnqwuf`, us-west-1),
  migrations 0001-0004 applied, private `releases` bucket created.
- **Vercel:** project `vc_composer2` (Root Directory `web`, Next.js, Node 22),
  domains vc-composer.com and www.vc-composer.com (redirects to the bare
  domain). Set so far: `NEXT_PUBLIC_SITE_URL`, `NEXT_PUBLIC_SUPABASE_URL`,
  `NEXT_PUBLIC_SUPABASE_ANON_KEY`, `RELEASE_BUCKET`, `RESEND_FROM_ADDRESS`.
- **Still to add** (secrets, by the owner, in Vercel > Settings > Environment
  Variables): `SUPABASE_SERVICE_ROLE_KEY` (Supabase > Project Settings > API
  Keys), `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRET`,
  `STRIPE_PRICE_ID_MONTHLY`, `STRIPE_PRICE_ID_YEARLY`, `RESEND_API_KEY`,
  `NEXT_PUBLIC_TURNSTILE_SITE_KEY`, `TURNSTILE_SECRET_KEY`, `RATE_LIMIT_SALT`
  (any long random string). Then redeploy.
- **Supabase Auth:** Site URL `https://vc-composer.com`; redirect URL
  `https://vc-composer.com/auth/callback`.
