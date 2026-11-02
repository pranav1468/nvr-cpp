pragma Singleton
import QtQuick 2.15

/**
 * Enterprise Surveillance UI Design System & Tokens
 * Exact synchronization with web console (web/app.css) variables and tokens
 */
QtObject {
    // Surfaces & Backgrounds (Enterprise Light Console)
    readonly property color bgBase: "#f7fafd"
    readonly property color bgApp: "#f7fafd"
    readonly property color bgSurface: "#ffffff"
    readonly property color bgCard: "#ffffff"
    readonly property color bgCardHover: "#f8fafc"
    readonly property color bgElevated: "#ffffff"
    readonly property color bgInput: "#ffffff"
    readonly property color bgModal: "#ffffff"
    readonly property color bgTileStage: "#0f172a"

    // Secondary & Accent Backgrounds
    readonly property color bgSecondary: "#f1f5f9"
    readonly property color bgAccent: "#f1f5f9"
    readonly property color bgBadgePrimary: "#e0e7ee"
    readonly property color bgBadgeBlue: "#e0eefe"

    // Borders & Dividers
    readonly property color borderSubtle: "#e2e8f0"
    readonly property color borderLight: "#cbd5e1"
    readonly property color borderHover: "#1266d4"
    readonly property color borderFocus: "#1266d4"

    // Typography Colors
    readonly property color textPrimary: "#0f172a"
    readonly property color textSecondary: "#475569"
    readonly property color textMuted: "#64748b"
    readonly property color textDim: "#94a3b8"
    readonly property color textAccent: "#1266d4"

    // Brand Accents
    readonly property color accentBlue: "#1266d4"
    readonly property color accentBlueHover: "#0e52ab"
    readonly property color accentCyan: "#0284c7"
    readonly property color accentIndigo: "#4f46e5"
    readonly property color accentPrimary: "#1266d4"
    readonly property color accentPrimaryFg: "#ffffff"

    // Telemetry & State Colors
    readonly property color statusOnline: "#16a34a"
    readonly property color statusOnlineGlow: "#15803d"
    readonly property color statusRec: "#dc2626"
    readonly property color statusRecGlow: "#b91c1c"
    readonly property color statusDanger: "#dc2626"
    readonly property color statusWarning: "#f59e0b"
    readonly property color statusConnecting: "#0284c7"
    readonly property color statusOffline: "#94a3b8"

    // Typography
    readonly property string fontDisplay: "Sora, 'Segoe UI', Roboto, sans-serif"
    readonly property string fontSans: "Manrope, 'Segoe UI', Roboto, sans-serif"
    readonly property string fontMono: "'IBM Plex Mono', 'SF Mono', Consolas, monospace"

    // Radii
    readonly property int radiusSm: 4
    readonly property int radiusMd: 6
    readonly property int radiusLg: 8
    readonly property int radiusXl: 12
    readonly property int radius2Xl: 16
}
