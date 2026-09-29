import styles from './shell.module.css';

// Present on every screen: nothing in Chronos involves real money.
export function PaperBanner() {
  return (
    <div className={styles.paperBanner} role="note" aria-label="Paper trading notice">
      <span className={styles.paperMark}>PAPER TRADING</span>
      <span className={styles.paperText}>
        Simulated orders against recorded market data. No real money, broker, or exchange is involved.
      </span>
    </div>
  );
}
