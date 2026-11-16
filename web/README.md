# vc-composer.com

The VC Composer 2 storefront: the marketing site, accounts, Stripe
subscriptions, licence delivery, installer downloads (licensed stable builds
and a free preview), and the admin console. It is the commerce half of VC
Writer's site, ported and renamed; the desktop app (the rest of this
repository) talks to it through the endpoints in [docs/api.md](docs/api.md).

Next.js 14 (app router) and TypeScript, Supabase (`@supabase/ssr`,
`supabase-js`), Stripe, Resend and zod, tested with vitest. Hosted on Vercel
with Root Directory `web`.

## What is where

| Path | What |
| --- | --- |
| `src/domain/` | Pure rules, no server: licences, activation, subscriptions and standing, discounts, given licences, the download gate. Shared by routes and pages. |
| `src/lib/` | Server modules: Supabase and Stripe clients, fulfilment, activation, downloads, email and its templates, rate limiting, Turnstile, admin console reads. |
| `src/app/api/` | Route handlers: checkout, Stripe webhook, downloads, releases, licence activation/standing/devices, account, auth, discounts, admin. |
| `src/app/(site)/` | Pages: home, download (with the free preview), sign in, account, purchase complete, collaboration, privacy, terms, admin. |
| `src/middleware.ts` | Session refresh and the nonce-based content security policy (not on `/api/stripe`). |
| `supabase/migrations/` | The database, in four numbered files. |
| `scripts/publish-release.mjs` | Uploads an installer and records it, inactive. |
| `docs/` | [Deployment](docs/deployment.md) and the [desktop API contract](docs/api.md). |

## How selling works

- Two recurring plans, monthly and yearly. Price ids come from
  `STRIPE_PRICE_ID_MONTHLY` / `STRIPE_PRICE_ID_YEARLY`; amounts are read live
  from Stripe. There are no prices in the code.
- Checkout stamps `metadata.product = vc-composer`. The Stripe account is
  shared with other VC Software products, and the webhook acts only on objects
  with that stamp.
- The webhook issues one `VCC-XXXXX-XXXXX-XXXXX-XXXXX` licence per checkout
  (idempotent on the session id and on `stripe_webhook_events`), carries
  renewals and cancellations onto it, and revokes it on a refund or dispute.
- Entitlement is always decided server-side from the `licenses` row. Stable
  downloads need a live licence; preview downloads need only a signed-in
  account. Every download is a short-lived signed URL.
- Collaboration will be a separate subscription; `/collaboration` says so and
  there is no billing code for it yet.

## Local development

Node 20 or later.

```sh
cd web
npm ci
cp .env.example .env.local   # fill in test-mode values; see docs/deployment.md
npm run dev                  # http://localhost:3000
```

Without any `.env.local` the public pages still render; sign-in, checkout and
downloads need their keys. To receive webhooks locally use the Stripe CLI:
`stripe listen --forward-to localhost:3000/api/stripe/webhook` and put the
secret it prints in `STRIPE_WEBHOOK_SECRET`.

## Checks

```sh
npm run typecheck
npm test          # vitest; no network, no keys
npm run build     # succeeds with no environment variables
```

CI runs all four (with `npm ci`) in the `web` job of `.github/workflows/ci.yml`.

`EMAIL_PREVIEW_DIR=/tmp/mail npm test` also writes the rendered emails to that
folder for checking in a browser.

## Becoming an admin, publishing builds

See [docs/deployment.md](docs/deployment.md) sections 6 and 7.
