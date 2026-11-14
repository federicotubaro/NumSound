/*
 * NumSound - Harmonic Sequence Synthesizer & Stereo Panorama (v1.1)
 * Required Notice: Copyright (c) 2026 Federico Tubaro (https://github.com)
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

import java.util.Arrays;
import java.util.Locale;
import javax.sound.midi.*;
import javax.sound.sampled.*;
import javax.swing.*;
import javax.swing.border.TitledBorder;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;
import java.awt.*;

public class NumSound {

    private static final float SAMPLE_RATE = 44100.0f;
    private static final int BUFFER_SAMPLES = 131072;
    private static final int MAX_OSCI = 256;

    private static volatile boolean mostraOndeSingole = true;
    private static volatile double cutoffLP = 16000.0;
    private static volatile int formaOnda = 0;
    private static volatile boolean isPlaying = false;
    private static volatile boolean triggerAttivo = true;
    private static volatile double sfasamentoNorm = 0.0;
    private static volatile boolean resetFasiRichiesto = false;

    private static volatile boolean hold1Attivo = false;
    private static volatile boolean hold2Attivo = false;
    private static volatile double attackSec = 0.1;
    private static volatile double releaseSec = 0.3;

    private static volatile int temperamentoAttivo = 0;
    private static volatile int notaRifAttiva = 0;
    private static volatile double freqRifHz = 256.0;

    private static volatile double[] frequenzeAttive = new double[0];
    private static volatile int[] cifreAttive = new int[0];
    private static volatile String sequenzaCorrente = "";
    private static volatile double frequenzaFondamentale = 64.0;
    private static volatile double[] latestSnapshot = new double[BUFFER_SAMPLES];

    private static class Voice {
        boolean gate = false;
        int midiNote = -1;
        double baseFreq = 64.0;
        double envelope = 0.0;
        double[] fasi = new double[MAX_OSCI];
        long triggerOrder = 0;
    }

    private static final Object voiceLock = new Object();
    private static final Voice[] voices = new Voice[]{new Voice(), new Voice()};
    private static long globalNoteCounter = 0;

    private static final String[] NOMI_NOTE = {
        "DO", "REb", "RE", "MIb", "MI", "FA", "FA#", "SOL", "LAb", "LA", "SIb", "SI"
    };

    private static final double[] PITAGORICO = {
        1.0, 256.0 / 243.0, 9.0 / 8.0, 32.0 / 27.0, 81.0 / 64.0, 4.0 / 3.0,
        729.0 / 512.0, 3.0 / 2.0, 128.0 / 81.0, 27.0 / 16.0, 16.0 / 9.0, 243.0 / 128.0
    };

    private static final double[] MESOTONICO = {
        1.0, 8.0 * Math.pow(5.0, -1.25), Math.sqrt(5.0) / 2.0, 4.0 * Math.pow(5.0, -0.75),
        1.25, 2.0 * Math.pow(5.0, -0.25), Math.pow(5.0, 1.5) / 8.0, Math.pow(5.0, 0.25),
        1.6, Math.pow(5.0, 0.75) / 2.0, 4.0 / Math.sqrt(5.0), Math.pow(5.0, 1.25) / 4.0
    };

    private static String getNomeTemperamento(int idx) {
        if (idx == 1) return "Equabile";
        if (idx == 2) return "V. Galilei";
        if (idx == 3) return "Mesotonico";
        return "Pitagorico";
    }

    private static String getNomeRif(int idx) {
        return (idx == 1) ? "LA3" : "DO3";
    }

    private static double getRapportoTemperamento(int tempIdx, int semitono) {
        if (tempIdx == 1) return Math.pow(2.0, semitono / 12.0);
        if (tempIdx == 2) return Math.pow(18.0 / 17.0, semitono);
        if (tempIdx == 3) return MESOTONICO[semitono];
        return PITAGORICO[semitono];
    }

    private static double getFreqDo3() {
        return (notaRifAttiva == 0) ? freqRifHz : (freqRifHz / getRapportoTemperamento(temperamentoAttivo, 9));
    }

    private static String calcolaNotaTemperata(double freq) {
        if (freq <= 0.0) return "--";
        double refDo2 = getFreqDo3() / 2.0;
        if (refDo2 <= 0.0) return "--";

        int ottava = 2;
        double freqNorm = freq;
        while (freqNorm < refDo2) { freqNorm *= 2.0; ottava--; }
        while (freqNorm >= refDo2 * 2.0) { freqNorm /= 2.0; ottava++; }

        double rapportoCorrente = freqNorm / refDo2;
        int indiceMigliore = 0;
        double minDiffCents = Double.MAX_VALUE;
        double freqNotaEsatta = refDo2;

        for (int i = 0; i < 12; i++) {
            double r = getRapportoTemperamento(temperamentoAttivo, i);
            double diffCents = 1200.0 * Math.abs(Math.log(rapportoCorrente / r) / Math.log(2.0));
            if (diffCents < minDiffCents) {
                minDiffCents = diffCents;
                indiceMigliore = i;
                freqNotaEsatta = r * refDo2 * Math.pow(2.0, ottava - 2);
            }
        }
        double diffDoSup = 1200.0 * Math.abs(Math.log(rapportoCorrente / 2.0) / Math.log(2.0));
        if (diffDoSup < minDiffCents) {
            minDiffCents = diffDoSup;
            indiceMigliore = 0;
            ottava++;
            freqNotaEsatta = refDo2 * Math.pow(2.0, ottava - 2);
        }

        String nomeNota = NOMI_NOTE[indiceMigliore] + ottava;
        return String.format(Locale.US, (minDiffCents < 1.5) ? "%s (%.1f Hz)" : "~%s (%.1f Hz)", nomeNota, freqNotaEsatta);
    }

    private static Color calcolaColoreDaFrequenza(double freq) {
        if (freq <= 0) return Color.GRAY;
        double semitoniDaDo1 = 12.0 * (Math.log(freq / 64.0) / Math.log(2.0));
        double posOttava = ((semitoniDaDo1 % 12.0) + 12.0) % 12.0 / 12.0;
        float hue = (float) ((0.36 + posOttava * 1.12) % 1.0);
        double ottava = semitoniDaDo1 / 12.0;
        float brightness = (float) Math.max(0.35, Math.min(1.0, 0.45 + ottava * 0.16));
        float saturation = (float) Math.max(0.45, Math.min(0.95, 0.95 - Math.max(0, ottava - 2.5) * 0.18));
        return Color.getHSBColor(hue, saturation, brightness);
    }
    
    private static JPanel creaGruppo(String titolo, Color titleColor) {
        JPanel p = new JPanel(new FlowLayout(FlowLayout.CENTER, 6, 2));
        p.setBackground(new Color(21, 26, 33));
        TitledBorder tb = BorderFactory.createTitledBorder(
                BorderFactory.createLineBorder(new Color(52, 66, 84)), titolo);
        tb.setTitleColor(titleColor);
        tb.setTitleFont(new Font("SansSerif", Font.BOLD, 10));
        p.setBorder(tb);
        return p;
    }

    public static void main(String[] args) {
        System.setProperty("apple.awt.application.name", "NumSound");
        try { UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName()); } catch (Exception ignored) {}
        SwingUtilities.invokeLater(NumSound::creaInterfacciaCompleta);
    }

    private static void creaInterfacciaCompleta() {
        JFrame frame = new JFrame("NumSound — v. 1.1 - (c)2026 Federico Tubaro");
        frame.setSize(1360, 720);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setLocationRelativeTo(null);
        frame.setLayout(new BorderLayout());

        // --- BARRA SUPERIORE ---
        JPanel barraSuperiore = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 6));
        barraSuperiore.setBackground(new Color(26, 32, 40));
        barraSuperiore.setBorder(BorderFactory.createMatteBorder(0, 0, 1, 0, new Color(45, 55, 68)));

        // 1. SEQUENZA ARMONICI
        JPanel grpSeq = creaGruppo("SEQUENZA ARMONICI", new Color(155, 180, 205));
        JTextField txtSequenza = new JTextField("", 14);
        txtSequenza.setFont(new Font("Monospaced", Font.BOLD, 16));
        txtSequenza.setHorizontalAlignment(JTextField.CENTER);
        grpSeq.add(txtSequenza);

        // 2. FREQ. (Hz) CIFRA 1
        JPanel grpBase = creaGruppo("FREQ. (Hz) CIFRA 1", new Color(100, 220, 170));
        JTextField txtFondamentale = new JTextField("64", 7);
        txtFondamentale.setFont(new Font("Monospaced", Font.BOLD, 16));
        txtFondamentale.setHorizontalAlignment(JTextField.CENTER);
        grpBase.add(txtFondamentale);

        // 3. NOTE riferite a TEMPERAMENTO (V1 sopra / V2 sotto)
        JPanel grpNote = creaGruppo("NOTE riferite a TEMPERAMENTO", new Color(155, 180, 205));
        JPanel pnlDueNote = new JPanel(new GridLayout(2, 1, 0, 1));
        pnlDueNote.setBackground(new Color(16, 21, 27));
        pnlDueNote.setBorder(BorderFactory.createCompoundBorder(
                BorderFactory.createLineBorder(new Color(55, 75, 90)),
                BorderFactory.createEmptyBorder(2, 6, 2, 6)));
        pnlDueNote.setPreferredSize(new Dimension(150, 34));

        JLabel lblNota1 = new JLabel("V1: " + calcolaNotaTemperata(64.0));
        lblNota1.setForeground(new Color(100, 220, 170));
        lblNota1.setFont(new Font("SansSerif", Font.BOLD, 11));

        JLabel lblNota2 = new JLabel("V2: --");
        lblNota2.setForeground(new Color(130, 200, 255));
        lblNota2.setFont(new Font("SansSerif", Font.BOLD, 11));

        pnlDueNote.add(lblNota1);
        pnlDueNote.add(lblNota2);

        JButton btnTemp = new JButton(getNomeTemperamento(temperamentoAttivo));
        btnTemp.setFont(new Font("SansSerif", Font.BOLD, 12));
        btnTemp.setFocusPainted(false);
        btnTemp.addActionListener(e -> {
            temperamentoAttivo = (temperamentoAttivo + 1) % 4;
            btnTemp.setText(getNomeTemperamento(temperamentoAttivo));
        });
        grpNote.add(pnlDueNote);
        grpNote.add(btnTemp);

        // 4. DIAPASON
        JPanel grpDiap = creaGruppo("DIAPASON", new Color(130, 200, 255));
        JButton btnRifNota = new JButton(getNomeRif(notaRifAttiva));
        btnRifNota.setFont(new Font("SansSerif", Font.BOLD, 12));
        btnRifNota.setFocusPainted(false);
        JTextField txtRifHz = new JTextField("256", 5);
        txtRifHz.setFont(new Font("Monospaced", Font.BOLD, 15));
        txtRifHz.setHorizontalAlignment(JTextField.CENTER);

        btnRifNota.addActionListener(e -> {
            notaRifAttiva = (notaRifAttiva + 1) % 2;
            btnRifNota.setText(getNomeRif(notaRifAttiva));
            if (notaRifAttiva == 1 && txtRifHz.getText().trim().equals("256")) {
                txtRifHz.setText("432");
                freqRifHz = 432.0;
            } else if (notaRifAttiva == 0 && (txtRifHz.getText().trim().equals("432") || txtRifHz.getText().trim().equals("440"))) {
                txtRifHz.setText("256");
                freqRifHz = 256.0;
            }
        });
        txtRifHz.getDocument().addDocumentListener(new DocumentListener() {
            private void upd() {
                try {
                    double v = Double.parseDouble(txtRifHz.getText().trim().replace(",", "."));
                    if (v > 10.0 && v < 2000.0) freqRifHz = v;
                } catch (Exception ignored) {}
            }
            @Override public void insertUpdate(DocumentEvent e) { upd(); }
            @Override public void removeUpdate(DocumentEvent e) { upd(); }
            @Override public void changedUpdate(DocumentEvent e) { upd(); }
        });
        grpDiap.add(btnRifNota);
        grpDiap.add(txtRifHz);

        // 5. HOLD VOCI (H1 / H2)
        JPanel grpHold = creaGruppo("HOLD VOCI", new Color(155, 180, 205));
        JToggleButton btnH1 = new JToggleButton("H1:OFF", false);
        JToggleButton btnH2 = new JToggleButton("H2:OFF", false);
        btnH1.setFont(new Font("SansSerif", Font.BOLD, 12));
        btnH2.setFont(new Font("SansSerif", Font.BOLD, 12));
        btnH1.setFocusPainted(false);
        btnH2.setFocusPainted(false);
        btnH1.addActionListener(e -> {
            hold1Attivo = btnH1.isSelected();
            btnH1.setText(hold1Attivo ? "H1:ON" : "H1:OFF");
        });
        btnH2.addActionListener(e -> {
            hold2Attivo = btnH2.isSelected();
            btnH2.setText(hold2Attivo ? "H2:ON" : "H2:OFF");
        });
        grpHold.add(btnH1);
        grpHold.add(btnH2);

        // 6. ATTACCO & RILASCIO (0-10s)
        JPanel grpEnv = creaGruppo("ATTACCO / RILASCIO (0-10s)", new Color(155, 180, 205));
        JSlider sldAtt = new JSlider(0, 100, 1);
        sldAtt.setPreferredSize(new Dimension(75, 22));
        sldAtt.setOpaque(false);
        JTextField txtAtt = new JTextField("0.1", 3);
        txtAtt.setFont(new Font("Monospaced", Font.BOLD, 13));
        txtAtt.setHorizontalAlignment(JTextField.CENTER);

        JSlider sldRel = new JSlider(0, 100, 3);
        sldRel.setPreferredSize(new Dimension(75, 22));
        sldRel.setOpaque(false);
        JTextField txtRel = new JTextField("0.3", 3);
        txtRel.setFont(new Font("Monospaced", Font.BOLD, 13));
        txtRel.setHorizontalAlignment(JTextField.CENTER);

        sldAtt.addChangeListener(e -> {
            attackSec = sldAtt.getValue() / 10.0;
            txtAtt.setText(String.format(Locale.US, "%.1f", attackSec));
        });
        txtAtt.addActionListener(e -> {
            try {
                double v = Math.max(0.0, Math.min(10.0, Double.parseDouble(txtAtt.getText().trim().replace(",", "."))));
                attackSec = v;
                sldAtt.setValue((int) Math.round(v * 10.0));
            } catch (Exception ignored) {}
        });

        sldRel.addChangeListener(e -> {
            releaseSec = sldRel.getValue() / 10.0;
            txtRel.setText(String.format(Locale.US, "%.1f", releaseSec));
        });
        txtRel.addActionListener(e -> {
            try {
                double v = Math.max(0.0, Math.min(10.0, Double.parseDouble(txtRel.getText().trim().replace(",", "."))));
                releaseSec = v;
                sldRel.setValue((int) Math.round(v * 10.0));
            } catch (Exception ignored) {}
        });

        grpEnv.add(new JLabel("A:") {{ setForeground(Color.LIGHT_GRAY); }});
        grpEnv.add(sldAtt);
        grpEnv.add(txtAtt);
        grpEnv.add(new JLabel("R:") {{ setForeground(Color.LIGHT_GRAY); }});
        grpEnv.add(sldRel);
        grpEnv.add(txtRel);

        barraSuperiore.add(grpSeq);
        barraSuperiore.add(grpBase);
        barraSuperiore.add(grpNote);
        barraSuperiore.add(grpDiap);
        barraSuperiore.add(grpHold);
        barraSuperiore.add(grpEnv);
        // --- CENTRO: Oscilloscopio + Spettro Panorama Stereo ---
        PannelloOnda pannello = new PannelloOnda();

        // --- BARRA INFERIORE ---
        JPanel barraInferiore = new JPanel(new FlowLayout(FlowLayout.CENTER, 14, 10));
        barraInferiore.setBackground(new Color(26, 32, 40));
        barraInferiore.setBorder(BorderFactory.createMatteBorder(1, 0, 0, 0, new Color(45, 55, 68)));

        JToggleButton btnSingole = new JToggleButton("Singole: ON", true);
        btnSingole.setFont(new Font("SansSerif", Font.BOLD, 13));
        btnSingole.setFocusPainted(false);
        btnSingole.addActionListener(e -> {
            mostraOndeSingole = btnSingole.isSelected();
            btnSingole.setText(mostraOndeSingole ? "Singole: ON" : "Singole: OFF");
        });

        JToggleButton btnTrigger = new JToggleButton("Trigger: ON", true);
        btnTrigger.setFont(new Font("SansSerif", Font.BOLD, 13));
        btnTrigger.setFocusPainted(false);
        btnTrigger.addActionListener(e -> {
            triggerAttivo = btnTrigger.isSelected();
            btnTrigger.setText(triggerAttivo ? "Trigger: ON" : "Trigger: OFF");
        });

        String[] tipiOnda = {"Sinusoide", "Triangolare", "Onda Quadra", "Dente di Sega"};
        JComboBox<String> comboOnda = new JComboBox<>(tipiOnda);
        comboOnda.setFont(new Font("SansSerif", Font.BOLD, 13));
        comboOnda.setFocusable(false);
        comboOnda.addActionListener(e -> formaOnda = comboOnda.getSelectedIndex());

        JLabel lblFase = new JLabel("Fase: 0°");
        lblFase.setForeground(new Color(210, 225, 240));
        lblFase.setFont(new Font("SansSerif", Font.BOLD, 13));
        lblFase.setPreferredSize(new Dimension(72, 20));

        JSlider sliderFase = new JSlider(0, 360, 0);
        sliderFase.setPreferredSize(new Dimension(130, 24));
        sliderFase.setOpaque(false);
        sliderFase.setFocusable(false);
        sliderFase.addChangeListener(e -> {
            int gradi = sliderFase.getValue();
            sfasamentoNorm = gradi / 360.0;
            lblFase.setText("Fase: " + gradi + "°");
        });

        JButton btnSync = new JButton("Sync 0°");
        btnSync.setFont(new Font("SansSerif", Font.BOLD, 12));
        btnSync.setFocusPainted(false);
        btnSync.addActionListener(e -> {
            sliderFase.setValue(0);
            sfasamentoNorm = 0.0;
            resetFasiRichiesto = true;
        });

        JLabel lblCutoff = new JLabel("LP: 16000 Hz");
        lblCutoff.setForeground(new Color(210, 225, 240));
        lblCutoff.setFont(new Font("SansSerif", Font.BOLD, 13));
        lblCutoff.setPreferredSize(new Dimension(110, 20));

        JSlider sliderLP = new JSlider(0, 1000, 1000);
        sliderLP.setPreferredSize(new Dimension(160, 24));
        sliderLP.setOpaque(false);
        sliderLP.setFocusable(false);
        sliderLP.addChangeListener(e -> {
            int val = sliderLP.getValue();
            if (val == 0) {
                cutoffLP = 0.0;
                lblCutoff.setText("LP: 0 Hz");
            } else {
                double freqTaglio = 10.0 * Math.pow(16000.0 / 10.0, val / 1000.0);
                cutoffLP = freqTaglio;
                lblCutoff.setText(String.format(Locale.US, "LP: %.0f Hz", freqTaglio));
            }
        });

        barraInferiore.add(btnSingole);
        barraInferiore.add(btnTrigger);
        barraInferiore.add(comboOnda);
        barraInferiore.add(lblFase);
        barraInferiore.add(sliderFase);
        barraInferiore.add(btnSync);
        barraInferiore.add(lblCutoff);
        barraInferiore.add(sliderLP);

		barraInferiore.setPreferredSize(new Dimension(1360, 48));
		frame.add(barraInferiore, BorderLayout.SOUTH);
		frame.setVisible(true);



        // Aggiornamento live di sequenza e base manuale anche in silenzio
        Runnable aggiornaLive = () -> {
            String pulita = txtSequenza.getText().replaceAll("[^1-9]", "");
            double nuovaBase = frequenzaFondamentale;
            try {
                double val = Double.parseDouble(txtFondamentale.getText().trim().replace(",", "."));
                if (val >= 1.0 && val <= 4096.0) nuovaBase = val;
            } catch (Exception ignored) {}
            aggiornaDatiSequenza(pulita, nuovaBase);
            synchronized (voiceLock) {
                if (voices[0].midiNote == -1 && voices[1].midiNote == -1) {
                    voices[0].baseFreq = nuovaBase;
                }
            }
        };

        txtSequenza.getDocument().addDocumentListener(new DocumentListener() {
            @Override public void insertUpdate(DocumentEvent e) { aggiornaLive.run(); }
            @Override public void removeUpdate(DocumentEvent e) { aggiornaLive.run(); }
            @Override public void changedUpdate(DocumentEvent e) { aggiornaLive.run(); }
        });
        txtFondamentale.getDocument().addDocumentListener(new DocumentListener() {
            @Override public void insertUpdate(DocumentEvent e) { aggiornaLive.run(); }
            @Override public void removeUpdate(DocumentEvent e) { aggiornaLive.run(); }
            @Override public void changedUpdate(DocumentEvent e) { aggiornaLive.run(); }
        });
        txtSequenza.addActionListener(e -> { btnH1.setSelected(true); hold1Attivo = true; btnH1.setText("H1:ON"); });
        txtFondamentale.addActionListener(e -> { btnH1.setSelected(true); hold1Attivo = true; btnH1.setText("H1:ON"); });

        frame.add(barraSuperiore, BorderLayout.NORTH);
        frame.add(pannello, BorderLayout.CENTER);
        frame.add(barraInferiore, BorderLayout.SOUTH);
        frame.revalidate();
        frame.setVisible(true);

        barraInferiore.setPreferredSize(new Dimension(1360, 48));

        frame.add(barraSuperiore, BorderLayout.NORTH);
        frame.add(pannello, BorderLayout.CENTER);
        frame.add(barraInferiore, BorderLayout.SOUTH);

        frame.revalidate();
        frame.setVisible(true);

        // Avvia thread audio sempre attivo (esegue silenzio quando gate=false e envelope=0)
        Thread audioThread = new Thread(NumSound::riproduciOndaSomma);
        audioThread.setDaemon(true);
        audioThread.setPriority(Thread.MAX_PRIORITY);
        audioThread.start();

        // Timer grafico 60 FPS: aggiorna anche le due etichette note V1 e V2
        Timer timer = new Timer(16, e -> {
            double f1, f2;
            boolean v2Used;
            synchronized (voiceLock) {
                f1 = voices[0].baseFreq;
                f2 = voices[1].baseFreq;
                v2Used = (voices[1].midiNote != -1) || (voices[1].envelope > 0.001);
            }
            lblNota1.setText("V1: " + calcolaNotaTemperata(f1));
            lblNota2.setText(v2Used ? ("V2: " + calcolaNotaTemperata(f2)) : "V2: --");
            pannello.repaint();
        });
        timer.start();

        inizializzaMidi(txtSequenza, txtFondamentale, sliderLP);
    }

    private static void inizializzaMidi(JTextField txtSequenza, JTextField txtFondamentale, JSlider sliderLP) {
        for (MidiDevice.Info info : MidiSystem.getMidiDeviceInfo()) {
            try {
                MidiDevice device = MidiSystem.getMidiDevice(info);
                if (device.getMaxTransmitters() != 0) {
                    device.open();
                    device.getTransmitter().setReceiver(new Receiver() {
                        @Override
                        public void send(MidiMessage message, long timeStamp) {
                            if (!(message instanceof ShortMessage)) return;
                            ShortMessage sm = (ShortMessage) message;
                            int cmd = sm.getCommand();
                            int d1 = sm.getData1();
                            int d2 = sm.getData2();

                            if (cmd == ShortMessage.NOTE_ON && d2 > 0) {
                                int diff = d1 - 60;
                                int ottave = Math.floorDiv(diff, 12);
                                int idxNota = Math.floorMod(diff, 12);
                                double freq = getFreqDo3() * Math.pow(2.0, ottave) * getRapportoTemperamento(temperamentoAttivo, idxNota);

                                synchronized (voiceLock) {
                                    int target = -1;
                                    for (int v = 0; v < 2; v++) if (voices[v].midiNote == d1) { target = v; break; }
                                    if (target == -1) for (int v = 0; v < 2; v++) if (!voices[v].gate && voices[v].envelope < 0.01) { target = v; break; }
                                    if (target == -1) for (int v = 0; v < 2; v++) if (!voices[v].gate) { target = v; break; }
                                    if (target == -1) target = (voices[0].triggerOrder <= voices[1].triggerOrder) ? 0 : 1;

                                    voices[target].midiNote = d1;
                                    voices[target].baseFreq = freq;
                                    voices[target].gate = true;
                                    voices[target].triggerOrder = ++globalNoteCounter;
                                }
                                SwingUtilities.invokeLater(() -> {
                                    if (txtSequenza.getText().replaceAll("[^1-9]", "").isEmpty()) txtSequenza.setText("1");
                                    txtFondamentale.setText(String.format(Locale.US, "%.2f", freq));
                                });
                            } else if (cmd == ShortMessage.NOTE_OFF || (cmd == ShortMessage.NOTE_ON && d2 == 0)) {
                                synchronized (voiceLock) {
                                    for (int v = 0; v < 2; v++) if (voices[v].midiNote == d1) voices[v].gate = false;
                                }
                            } else if (cmd == ShortMessage.CONTROL_CHANGE && d1 == 7) {
                                int sVal = (int) Math.round((d2 / 127.0) * 1000.0);
                                SwingUtilities.invokeLater(() -> sliderLP.setValue(sVal));
                            }
                        }
                        @Override public void close() {}
                    });
                }
            } catch (Exception ignored) {}
        }
    }
    private static void aggiornaDatiSequenza(String testoPulito, double nuovaBase) {
        int len = Math.min(testoPulito.length(), MAX_OSCI);
        double[] nuoveFreq = new double[len];
        int[] nuoveCifre = new int[len];
        StringBuilder seqPulita = new StringBuilder();

        for (int i = 0; i < len; i++) {
            int cifra = Character.getNumericValue(testoPulito.charAt(i));
            nuoveCifre[i] = cifra;
            nuoveFreq[i] = cifra * nuovaBase;
            seqPulita.append(cifra).append(" ");
        }
        frequenzaFondamentale = nuovaBase;
        cifreAttive = nuoveCifre;
        frequenzeAttive = nuoveFreq;
        sequenzaCorrente = seqPulita.toString().trim();
    }

    private static void riproduciOndaSomma() {
        AudioFormat format = new AudioFormat(SAMPLE_RATE, 16, 2, true, false);
        try (SourceDataLine line = AudioSystem.getSourceDataLine(format)) {
            line.open(format, 2048);
            line.start();

            int chunkSize = 64;
            byte[] audioBuffer = new byte[chunkSize * 4];
            double[] rollingHistory = new double[BUFFER_SAMPLES];
            double lpLeft1 = 0.0, lpLeft2 = 0.0, lpRight1 = 0.0, lpRight2 = 0.0;
            int contatoreGrafica = 0;

            while (true) {
                int[] cifre = cifreAttive;
                int numOsci = (cifre == null) ? 0 : cifre.length;

                boolean h1 = hold1Attivo;
                boolean h2 = hold2Attivo;

                boolean[] vGate = new boolean[2];
                double[] vBase = new double[2];
                double[] vEnv = new double[2];
                double[][] vFasi = new double[2][];

                synchronized (voiceLock) {
                    if (resetFasiRichiesto) {
                        Arrays.fill(voices[0].fasi, 0.0);
                        Arrays.fill(voices[1].fasi, 0.0);
                        resetFasiRichiesto = false;
                    }
                    for (int v = 0; v < 2; v++) {
                        boolean vHold = (v == 0) ? h1 : (h2 && voices[1].midiNote != -1);
                        vGate[v] = voices[v].gate || vHold;
                        vBase[v] = voices[v].baseFreq;
                        vEnv[v] = voices[v].envelope;
                        vFasi[v] = voices[v].fasi;
                    }
                }

                boolean anyActive = (numOsci > 0) && (vGate[0] || vEnv[0] > 0.0 || vGate[1] || vEnv[1] > 0.0);
                isPlaying = anyActive;

                if (!anyActive) {
                    Arrays.fill(audioBuffer, (byte) 0);
                    line.write(audioBuffer, 0, audioBuffer.length);
                    continue;
                }

                double[] gainLeft = new double[numOsci];
                double[] gainRight = new double[numOsci];
                for (int k = 0; k < numOsci; k++) {
                    double angolo = (numOsci == 1) ? (Math.PI / 4.0) : (k * (Math.PI / 2.0) / (numOsci - 1));
                    gainLeft[k] = Math.cos(angolo);
                    gainRight[k] = Math.sin(angolo);
                }

                System.arraycopy(rollingHistory, chunkSize, rollingHistory, 0, BUFFER_SAMPLES - chunkSize);

                int tipoOnda = formaOnda;
                double sfas = sfasamentoNorm;
                double attackStep = 1.0 / (SAMPLE_RATE * Math.max(0.005, attackSec));
                double releaseStep = 1.0 / (SAMPLE_RATE * Math.max(0.005, releaseSec));
                double alpha = (cutoffLP <= 0.0) ? 0.0 : ((1.0 / SAMPLE_RATE) / ((1.0 / (2.0 * Math.PI * cutoffLP)) + (1.0 / SAMPLE_RATE)));
                double fattoreNorm = Math.sqrt(numOsci * 1.5);

                for (int i = 0; i < chunkSize; i++) {
                    double sommaLeft = 0.0, sommaRight = 0.0;

                    for (int v = 0; v < 2; v++) {
                        if (vGate[v] && vEnv[v] < 1.0) vEnv[v] = Math.min(1.0, vEnv[v] + attackStep);
                        else if (!vGate[v] && vEnv[v] > 0.0) vEnv[v] = Math.max(0.0, vEnv[v] - releaseStep);
                        if (vEnv[v] <= 0.0) continue;

                        for (int k = 0; k < numOsci; k++) {
                            double fase = vFasi[v][k];
                            double distCentro = k - (numOsci - 1) / 2.0;
                            double fEff = fase - (distCentro * sfas);
                            fEff -= Math.floor(fEff);

                            double onda;
                            if (tipoOnda == 1) onda = 1.0 - 4.0 * Math.abs(fEff - 0.5);
                            else if (tipoOnda == 2) onda = (fEff < 0.5) ? 1.0 : -1.0;
                            else if (tipoOnda == 3) onda = (2.0 * fEff) - 1.0;
                            else onda = Math.sin(2.0 * Math.PI * fEff);

                            sommaLeft += onda * gainLeft[k] * vEnv[v];
                            sommaRight += onda * gainRight[k] * vEnv[v];

                            fase += (cifre[k] * vBase[v]) / SAMPLE_RATE;
                            if (fase >= 1.0) fase -= Math.floor(fase);
                            vFasi[v][k] = fase;
                        }
                    }

                    lpLeft1 += alpha * (sommaLeft - lpLeft1);
                    lpLeft2 += alpha * (lpLeft1 - lpLeft2);
                    sommaLeft = lpLeft2;
                    lpRight1 += alpha * (sommaRight - lpRight1);
                    lpRight2 += alpha * (lpRight1 - lpRight2);
                    sommaRight = lpRight2;

                    double normLeft = Math.tanh((sommaLeft / fattoreNorm) * 0.65) * 0.65;
                    double normRight = Math.tanh((sommaRight / fattoreNorm) * 0.65) * 0.65;
                    rollingHistory[(BUFFER_SAMPLES - chunkSize) + i] = ((normLeft + normRight) * 0.5) * 2.0;

                    short valLeft = (short) (normLeft * Short.MAX_VALUE);
                    short valRight = (short) (normRight * Short.MAX_VALUE);
                    audioBuffer[i * 4] = (byte) (valLeft & 0xFF);
                    audioBuffer[i * 4 + 1] = (byte) ((valLeft >> 8) & 0xFF);
                    audioBuffer[i * 4 + 2] = (byte) (valRight & 0xFF);
                    audioBuffer[i * 4 + 3] = (byte) ((valRight >> 8) & 0xFF);
                }

                synchronized (voiceLock) {
                    voices[0].envelope = vEnv[0];
                    voices[1].envelope = vEnv[1];
                }

                if (++contatoreGrafica % 8 == 0) latestSnapshot = rollingHistory.clone();
                line.write(audioBuffer, 0, audioBuffer.length);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private static class PannelloOnda extends JPanel {
        @Override
        protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g;
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
            int w = getWidth(), h = getHeight();
            int altezzaSpettro = Math.max(215, (int) (h * 0.40));
            int altezzaOnda = h - altezzaSpettro;
            int midY = altezzaOnda / 2;

            g2.setColor(Color.BLACK);
            g2.fillRect(0, 0, w, altezzaOnda);

            for (int i = 1; i < 16; i++) {
                int x = (int) Math.round((i / 16.0) * w);
                g2.setColor(i == 8 ? new Color(48, 64, 82) : new Color(32, 42, 54));
                g2.drawLine(x, 0, x, altezzaOnda);
            }
            double ampMaxPixel = altezzaOnda * 0.42;
            g2.setColor(new Color(32, 42, 54));
            for (int i = 1; i <= 4; i++) {
                int offY = (int) Math.round((i / 4.0) * ampMaxPixel);
                g2.drawLine(0, midY - offY, w, midY - offY);
                g2.drawLine(0, midY + offY, w, midY + offY);
            }
            g2.setColor(new Color(60, 80, 100));
            g2.drawLine(0, midY, w, midY);

            double[] vBase = new double[2];
            double[] vEnv = new double[2];
            boolean[] vUsed = new boolean[2];
            synchronized (voiceLock) {
                for (int v = 0; v < 2; v++) {
                    vBase[v] = voices[v].baseFreq;
                    vEnv[v] = voices[v].envelope;
                    vUsed[v] = (v == 0) || (voices[v].midiNote != -1) || (voices[v].envelope > 0.001);
                }
            }

            double[] samples = latestSnapshot;
            int[] cifre = cifreAttive;
            double[] freqs = frequenzeAttive;
            boolean playingNow = isPlaying;

            double triggerFreq = (vEnv[0] > 0.001) ? vBase[0] : ((vEnv[1] > 0.001) ? vBase[1] : frequenzaFondamentale);
            int periodoAttuale = (int) Math.round(SAMPLE_RATE / Math.max(1.0, triggerFreq));
            int displaySamples = Math.min(BUFFER_SAMPLES - periodoAttuale * 2 - 10, Math.max(100, periodoAttuale * 2));
            int startIdx = BUFFER_SAMPLES - displaySamples;

            if (triggerAttivo && playingNow && samples != null) {
                int searchEnd = BUFFER_SAMPLES - displaySamples;
                int searchStart = Math.max(1, searchEnd - periodoAttuale * 2);
                double peakLocal = 0.0;
                for (int i = searchStart; i < searchEnd; i++) peakLocal = Math.max(peakLocal, Math.abs(samples[i]));
                double armThreshold = -peakLocal * 0.25;
                boolean armed = false;
                for (int i = searchStart; i < searchEnd; i++) {
                    if (samples[i] < armThreshold) armed = true;
                    else if (armed && samples[i - 1] < 0.0 && samples[i] >= 0.0) { startIdx = i; break; }
                }
            }

            // Onde singole (tracce sbiadite in silenzio, piene durante il suono)
            if (mostraOndeSingole && cifre != null && cifre.length > 0) {
                int numOsci = cifre.length;
                double vEnv0 = vEnv[0];
                double vEnv1 = vEnv[1];
                boolean vUsed1 = vUsed[1];

                boolean show0 = true;
                boolean show1 = false;
                if (playingNow) {
                    show0 = vEnv0 > 0.001;
                    show1 = vEnv1 > 0.001;
                    if (!show0 && !show1) show0 = true;
                } else {
                    show0 = true;
                    show1 = vUsed1;
                }
                int numVociVis = (show0 ? 1 : 0) + (show1 ? 1 : 0);
                double scalaAmp = (0.65 / Math.sqrt(numOsci * (numVociVis > 1 ? 1.5 : 1.0))) * ampMaxPixel;

                for (int v = 0; v < 2; v++) {
                    boolean activeV = (v == 0) ? show0 : show1;
                    if (!activeV) continue;

                    float spessore = playingNow ? (0.7f + (float) (0.8 * vEnv[v])) : 0.7f;
                    int alpha = playingNow ? (int) Math.round(45.0 + 95.0 * vEnv[v]) : 45;
                    g2.setStroke(new BasicStroke(spessore));

                    for (int k = 0; k < numOsci; k++) {
                        Color c = calcolaColoreDaFrequenza(cifre[k] * vBase[v]);
                        g2.setColor(new Color(c.getRed(), c.getGreen(), c.getBlue(), alpha));
                        double stepFase = (cifre[k] * vBase[v]) / SAMPLE_RATE;
                        double faseOffset = -((k - (numOsci - 1) / 2.0) * sfasamentoNorm);
                        int pX = 0, pY = midY;
                        for (int i = 0; i < displaySamples; i++) {
                            double f = (i * stepFase) + faseOffset;
                            f -= Math.floor(f);
                            double val = (formaOnda == 1) ? (1.0 - 4.0 * Math.abs(f - 0.5))
                                       : (formaOnda == 2) ? ((f < 0.5) ? 1.0 : -1.0)
                                       : (formaOnda == 3) ? ((2.0 * f) - 1.0)
                                       : Math.sin(2.0 * Math.PI * f);
                            int x = (int) ((i / (double) (displaySamples - 1)) * w);
                            int y = midY - (int) (val * scalaAmp);
                            if (i > 0) g2.drawLine(pX, pY, x, y);
                            pX = x; pY = y;
                        }
                    }
            }

            // Onda somma spessa (4.2f)
            if (playingNow && samples != null) {
                Color colSum = Color.WHITE;
                if (freqs != null && freqs.length > 0) {
                    double rT = 0, gT = 0, bT = 0;
                    for (double f : freqs) { Color c = calcolaColoreDaFrequenza(f); rT += c.getRed(); gT += c.getGreen(); bT += c.getBlue(); }
                    double nC = Math.sqrt(freqs.length);
                    colSum = new Color(Math.min(255, (int) Math.round(rT / nC)), Math.min(255, (int) Math.round(gT / nC)), Math.min(255, (int) Math.round(bT / nC)));
                }
                g2.setColor(colSum);
                g2.setStroke(new BasicStroke(4.2f));
                int pX = 0, pY = midY - (int) (samples[startIdx] * ampMaxPixel);
                for (int i = 1; i < displaySamples; i++) {
                    int x = (int) ((i / (double) (displaySamples - 1)) * w);
                    int y = midY - (int) (samples[startIdx + i] * ampMaxPixel);
                    g2.drawLine(pX, pY, x, y);
                    pX = x; pY = y;
                }
            }
            // --- PANORAMA STEREO SPECULARE ---
            int topSpettro = altezzaOnda;
            g2.setColor(new Color(20, 26, 34));
            g2.fillRect(0, topSpettro, w, altezzaSpettro);
            g2.setColor(new Color(50, 65, 82));
            g2.setStroke(new BasicStroke(1.5f));
            g2.drawLine(0, topSpettro, w, topSpettro);

            int margineX = 75, areaUtileX = w - (margineX * 2);
            int zeroY = topSpettro + (altezzaSpettro - 18) / 2;
            int maxBarHalfH = ((altezzaSpettro - 18) / 2) - 34;

            g2.setColor(new Color(85, 108, 132));
            g2.drawLine(margineX - 25, zeroY, w - margineX + 25, zeroY);
            g2.setFont(new Font("SansSerif", Font.BOLD, 11));
            g2.setColor(new Color(140, 165, 190));
            String tit = "PANORAMA STEREO & OCCORRENZE ARMONICHE (VOCE 1 / VOCE 2)";
            g2.drawString(tit, (w - g2.getFontMetrics().stringWidth(tit)) / 2, topSpettro + 16);
            g2.setColor(new Color(100, 220, 170));
            g2.drawString("VOCE 1", 12, zeroY - 8);
            g2.setColor(new Color(130, 200, 255));
            g2.drawString("VOCE 2", 12, zeroY + 16);
            g2.setColor(new Color(120, 150, 180));
            g2.drawString("L (100% Left)", margineX - 35, h - 6);
            g2.drawString("C (Center)", (w / 2) - 26, h - 6);
            g2.drawString("R (100% Right)", w - margineX - 45, h - 6);

            if (cifre == null || cifre.length == 0) return;
			int[] occ = new int[10];
            int maxOcc = 1;
            for (int c : cifre) if (c >= 1 && c <= 9) maxOcc = Math.max(maxOcc, ++occ[c]);
            int numOsciSpettro = cifre.length;
            int barW = Math.max(10, Math.min(38, (areaUtileX / Math.max(1, numOsci)) - 10));

            for (int k = 0; k < numOsciSpettro; k++) {
                int c = cifre[k], count = occ[c];
                double normPan = (numOsciSpettro == 1) ? 0.5 : (k / (double) (numOsciSpettro - 1));
                int cx = margineX + (int) Math.round(normPan * areaUtileX);
                double ratio = count / (double) maxOcc;
                double vBase0 = vBase[0];
                double vBase1 = vBase[1];
                
                // VOCE 1 (su)
                double f1 = c * vBase0;
                int fullH1 = (int) Math.round(ratio * maxBarHalfH);
                int barH1 = (int) Math.round(fullH1 * vEnv0);
                Color col1 = calcolaColoreDaFrequenza(f1);
                g2.setColor(new Color(col1.getRed(), col1.getGreen(), col1.getBlue(), 30));
                g2.drawRect(cx - barW / 2, zeroY - fullH1, barW, fullH1);
                if (barH1 > 0) {
                    g2.setColor(new Color(col1.getRed(), col1.getGreen(), col1.getBlue(), 200));
                    g2.fillRoundRect(cx - barW / 2, zeroY - barH1, barW, barH1, 6, 6);
                }

                // VOCE 2 (giù)
                if (vUsed1) {
                    double f2 = c * vBase1;
                    int fullH2 = (int) Math.round(ratio * maxBarHalfH);
                    int barH2 = (int) Math.round(fullH2 * vEnv1);
                    Color col2 = calcolaColoreDaFrequenza(f2);
                    g2.setColor(new Color(col2.getRed(), col2.getGreen(), col2.getBlue(), 30));
                    g2.drawRect(cx - barW / 2, zeroY + 1, barW, fullH2);
                    if (barH2 > 0) {
                        g2.setColor(new Color(col2.getRed(), col2.getGreen(), col2.getBlue(), 200));
                        g2.fillRoundRect(cx - barW / 2, zeroY + 1, barW, barH2, 6, 6);
                    }

                g2.setFont(new Font("Monospaced", Font.BOLD, 13));
                g2.setColor(Color.WHITE);
                String lblC = String.valueOf(c);
                g2.drawString(lblC, cx - g2.getFontMetrics().stringWidth(lblC) / 2, zeroY + 4);

                if (numOsci <= 22) {
                    g2.setFont(new Font("SansSerif", Font.PLAIN, 9));
                    g2.setColor(new Color(195, 215, 235));
                    String infoT = (count > 1) ? String.format(Locale.US, "%.0fHz (x%d)", f1, count) : String.format(Locale.US, "%.0fHz", f1);
                    g2.drawString(infoT, cx - g2.getFontMetrics().stringWidth(infoT) / 2, zeroY - fullH1 - 3);
                }
            }
        }
 	}
  }
 }
}
