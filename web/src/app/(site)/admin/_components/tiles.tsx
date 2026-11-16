import { formatRevenue, type DashboardMetrics } from '@/lib/admin-console';

/**
 * The dashboard's stat tiles, in the order the spec lists them.
 *
 * Revenue over 30 days is the hero — the one figure the page leads with. A
 * tile that needs attention carries the words as well as a red border, so it
 * reads the same to everyone.
 */
export function StatTiles({ metrics }: { metrics: DashboardMetrics }) {
  const devices = metrics.activeDevices.windows + metrics.activeDevices.macos;
  return (
    <div className="stats" role="list">
      <Tile hero label="Revenue · 30 days" value={formatRevenue(metrics.revenue30)} />
      <Tile label="Revenue · 7 days" value={formatRevenue(metrics.revenue7)} />
      <Tile label="Orders · 30 days" value={String(metrics.paidOrders30)} detail="paid" />
      <Tile
        label="Refunds & disputes · 30 days"
        value={String(metrics.refundsAndDisputes30)}
        marked={metrics.refundsAndDisputes30 > 0}
        attention="Look in Stripe"
      />
      <Tile label="Active licences" value={String(metrics.activeLicenses)} />
      <Tile
        label="Activated devices"
        value={String(devices)}
        detail={`${metrics.activeDevices.windows} Windows · ${metrics.activeDevices.macos} macOS`}
      />
      <Tile
        label="Failed emails · 7 days"
        value={String(metrics.failedEmails7)}
        marked={metrics.failedEmails7 > 0}
        attention="See Email"
      />
      <Tile
        label="Current stable build"
        value={metrics.currentBuilds.windows ?? metrics.currentBuilds.macos ?? 'none'}
        detail={`Windows ${metrics.currentBuilds.windows ?? 'none published'} · macOS ${metrics.currentBuilds.macos ?? 'none published'}`}
        marked={!metrics.currentBuilds.windows || !metrics.currentBuilds.macos}
        attention="A platform has no stable build"
      />
      <Tile
        label="Free preview"
        value={metrics.previewBuilds.windows ?? metrics.previewBuilds.macos ?? 'none'}
        detail={`Windows ${metrics.previewBuilds.windows ?? 'none live'} · macOS ${metrics.previewBuilds.macos ?? 'none live'}`}
      />
    </div>
  );
}

function Tile({
  label,
  value,
  detail,
  hero = false,
  marked = false,
  attention,
}: {
  label: string;
  value: string;
  detail?: string;
  hero?: boolean;
  marked?: boolean;
  attention?: string;
}) {
  const className = ['stat', hero ? 'hero' : '', marked ? 'marked' : ''].filter(Boolean).join(' ');
  return (
    <div className={className} role="listitem">
      <span className="label">{label}</span>
      <span className="value">{value}</span>
      {detail ? <span className="detail">{detail}</span> : null}
      {marked && attention ? <span className="attention">{attention}</span> : null}
    </div>
  );
}
