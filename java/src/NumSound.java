/*
 * Num2Sound - Harmonic Sequence Synthesizer & Stereo Panorama (v1.0)
 * Copyright (c) 2026 [Your Name or Alias] - All Rights Reserved.
 *
 * LICENSE TERMS (FREE NON-COMMERCIAL USE):
 * 1. Use and Sharing: You are permitted to install, run, and share this software
 *    free of charge with anyone, provided that this copyright notice remains intact
 *    and clear credit is given to the original author.
 * 2. No Commercial Use: Selling, renting, distributing for a fee, or integrating
 *    this software (in whole or in part) into commercial products or services is
 *    strictly prohibited without a prior written agreement with the author.
 * 3. Commercial Licensing Inquiries: [Your Contact / Email or Website]
 *
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES, OR
 * OTHER LIABILITY ARISING FROM THE USE OF THIS SOFTWARE.
 */


import java.util.Arrays;
import javax.sound.midi.*;
import javax.sound.sampled.*;
import javax.swing.*;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;
import java.awt.*;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;

public class NumSound {

    private static volatile boolean mostraOndeSingole = true;
    private static volatile double cutoffLP = 16000.0; // Frequenza di taglio filtro LP in Hz (80 - 16000 Hz)
    private static volatile int formaOnda = 0; // 0 = Sinusoide, 1 = Dente di Sega, 2 = Quadra
    private static final float SAMPLE_RATE = 44100.0f;
    private static volatile boolean isPlaying = false;
    private static volatile boolean triggerAttivo = true;
    private static volatile double sfasamentoNorm = 0.0;
    private static volatile boolean resetFasiRichiesto = false;
    private static volatile double[] frequenzeAttive = new double[0];
    private static volatile int[] cifreAttive = new int[0];
    private static volatile String sequenzaCorrente = "";
    private static volatile double frequenzaFondamentale = 64.0;

    private static final int BUFFER_SAMPLES = 131072;
    private static volatile double[] latestSnapshot = new double[BUFFER_SAMPLES];
    private static Thread audioThread;

    // Rapporti esatti della scala pitagorica cromatica (12 note)
    private static final double[] RAPPORTI_PITAGORICI = {
        1.0,              // DO
        256.0 / 243.0,    // REb (Limma pitagorico)
        9.0 / 8.0,        // RE
        32.0 / 27.0,      // MIb
        81.0 / 64.0,      // MI (Ditono pitagorico)
        4.0 / 3.0,        // FA
        729.0 / 512.0,    // FA# (Tritono pitagorico)
        3.0 / 2.0,        // SOL
        128.0 / 81.0,     // LAb
        27.0 / 16.0,      // LA
        16.0 / 9.0,       // SIb
        243.0 / 128.0     // SI
    };
	
	// Colori pre-calcolati per ottava (stessa identica tonalità pura su DO e SOL)
    private static final Color[] COLORI_NOTE = {
        Color.GRAY,                   // 0 (non usato)
        new Color(18, 92, 30),        // 1: DO1 - Verde smeraldo scuro
        new Color(32, 158, 53),       // 2: DO2 - Verde smeraldo medio
        new Color(145, 22, 22),       // 3: SOL2 - Rosso rubino scuro
        new Color(52, 224, 81),       // 4: DO3 - Verde smeraldo brillante
        new Color(139, 82, 255),      // 5: MI3 - Indaco/Violetto
        new Color(245, 76, 76),       // 6: SOL3 - Rosso rubino chiaro
        new Color(255, 172, 20),      // 7: SIb3 - Arancio Ambra
        new Color(125, 250, 146),     // 8: DO4 - Verde smeraldo pastello
        new Color(25, 200, 255)       // 9: RE4 - Azzurro Ciano
    };



    private static final String[] NOMI_NOTE = {
        "DO", "REb", "RE", "MIb", "MI", "FA", "FA#", "SOL", "LAb", "LA", "SIb", "SI"
    };

    public static void main(String[] args) {
    	System.setProperty("apple.awt.application.name", "NumSound");
        try {
            UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
        } catch (Exception ignored) {}

        SwingUtilities.invokeLater(NumSound::creaInterfacciaCompleta);
    }

    private static void creaInterfacciaCompleta() {
        JFrame frame = new JFrame("NumSound — v. 1 - (c)2026 Federico Tubaro");
        frame.setSize(1160, 660);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setLocationRelativeTo(null);
        frame.setLayout(new BorderLayout());

        frame.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                isPlaying = false;
            }
        });

        // --- BARRA SUPERIORE: Sequenza + Fondamentale + Nota Pitagorica ---
        JPanel barraSuperiore = new JPanel(new FlowLayout(FlowLayout.CENTER, 12, 12));
        barraSuperiore.setBackground(new Color(26, 32, 40));
        barraSuperiore.setBorder(BorderFactory.createMatteBorder(0, 0, 1, 0, new Color(45, 55, 68)));

        JLabel lblSequenza = new JLabel("Sequenza (1-9):");
        lblSequenza.setForeground(new Color(210, 225, 240));
        lblSequenza.setFont(new Font("SansSerif", Font.BOLD, 14));

        JTextField txtSequenza = new JTextField("", 24);
        txtSequenza.setFont(new Font("Monospaced", Font.BOLD, 16));
        txtSequenza.setHorizontalAlignment(JTextField.CENTER);

        // Espansione dinamica della casella se si inseriscono molte cifre
        txtSequenza.getDocument().addDocumentListener(new DocumentListener() {
            private void aggiornaLarghezza() {
                int lunghezza = Math.max(24, txtSequenza.getText().length() + 4);
                if (txtSequenza.getColumns() != lunghezza) {
                    txtSequenza.setColumns(lunghezza);
                    barraSuperiore.revalidate();
                }
            }
            @Override public void insertUpdate(DocumentEvent e) { aggiornaLarghezza(); }
            @Override public void removeUpdate(DocumentEvent e) { aggiornaLarghezza(); }
            @Override public void changedUpdate(DocumentEvent e) { aggiornaLarghezza(); }
        });

        JLabel lblFondamentale = new JLabel("Base (Hz):");
        lblFondamentale.setForeground(new Color(210, 225, 240));
        lblFondamentale.setFont(new Font("SansSerif", Font.BOLD, 14));

        JTextField txtFondamentale = new JTextField("64", 10);
        txtFondamentale.setFont(new Font("Monospaced", Font.BOLD, 16));
        txtFondamentale.setHorizontalAlignment(JTextField.CENTER);

        JLabel lblNotaPitagorica = new JLabel(calcolaNotaPitagorica(64.0));
        lblNotaPitagorica.setForeground(new Color(100, 220, 170));
        lblNotaPitagorica.setFont(new Font("SansSerif", Font.BOLD, 14));
        lblNotaPitagorica.setBorder(BorderFactory.createCompoundBorder(
                BorderFactory.createLineBorder(new Color(55, 75, 90)),
                BorderFactory.createEmptyBorder(4, 10, 4, 10)
        ));

        JButton btnPlayStop = new JButton("Avvia Suono");
        btnPlayStop.setFont(new Font("SansSerif", Font.BOLD, 14));
        btnPlayStop.setFocusPainted(false);

        barraSuperiore.add(lblSequenza);
        barraSuperiore.add(txtSequenza);
        barraSuperiore.add(lblFondamentale);
        barraSuperiore.add(txtFondamentale);
        barraSuperiore.add(lblNotaPitagorica);
        barraSuperiore.add(btnPlayStop);

        // --- CENTRO: Oscilloscopio + Spettro Panorama Stereo ---
        PannelloOnda pannello = new PannelloOnda();

        // --- BARRA INFERIORE: Controlli onda ---
        JPanel barraInferiore = new JPanel(new FlowLayout(FlowLayout.CENTER, 15, 10));
        barraInferiore.setBackground(new Color(26, 32, 40));
        barraInferiore.setBorder(BorderFactory.createMatteBorder(1, 0, 0, 0, new Color(45, 55, 68)));

        JToggleButton btnTrigger = new JToggleButton("Trigger: ON", true);
        btnTrigger.setFont(new Font("SansSerif", Font.BOLD, 13));
        btnTrigger.setFocusPainted(false);
        btnTrigger.addActionListener(e -> {
            triggerAttivo = btnTrigger.isSelected();
            btnTrigger.setText(triggerAttivo ? "Trigger: ON" : "Trigger: OFF");
        });
        
        JToggleButton btnSingole = new JToggleButton("Singole: ON", true);
        btnSingole.setFont(new Font("SansSerif", Font.BOLD, 13));
        btnSingole.setFocusPainted(false);
        btnSingole.addActionListener(e -> {
        mostraOndeSingole = btnSingole.isSelected();
        btnSingole.setText(mostraOndeSingole ? "Singole: ON" : "Singole: OFF");
        });
        barraInferiore.add(btnSingole);
        
        JLabel lblOnda = new JLabel("Forma d'onda:");
        lblOnda.setForeground(new Color(210, 225, 240));
        lblOnda.setFont(new Font("SansSerif", Font.BOLD, 13));

        String[] tipiOnda = {"Sinusoide", "Triangolare", "Onda Quadra", "Dente di Sega"};
        JComboBox<String> comboOnda = new JComboBox<>(tipiOnda);
        comboOnda.setFont(new Font("SansSerif", Font.BOLD, 13));
        comboOnda.setFocusable(false);
        comboOnda.addActionListener(e -> formaOnda = comboOnda.getSelectedIndex());
		
		JLabel lblCutoff = new JLabel("Filtro LP: 16000 Hz");
        lblCutoff.setForeground(new Color(210, 225, 240));
        lblCutoff.setFont(new Font("SansSerif", Font.BOLD, 13));
        lblCutoff.setPreferredSize(new Dimension(135, 20));

        // Slider da 0 a 1000 mappato logaritmicamente tra 80 Hz e 16000 Hz
        JSlider sliderLP = new JSlider(0, 1000, 1000);
        sliderLP.setPreferredSize(new Dimension(180, 24));
        sliderLP.setOpaque(false);
        sliderLP.setFocusable(false);
                sliderLP.addChangeListener(e -> {
            int val = sliderLP.getValue();
            if (val == 0) {
                cutoffLP = 0.0;
                lblCutoff.setText("Filtro LP: 0 Hz");
            } else {
                double norm = val / 1000.0;
                // Scala logaritmica fluida da 10 Hz fino a 16000 Hz
                double freqTaglio = 10.0 * Math.pow(16000.0 / 10.0, norm);
                cutoffLP = freqTaglio;
                lblCutoff.setText(String.format("Filtro LP: %.0f Hz", freqTaglio));
            }
        });
        
		

        JLabel lblFase = new JLabel("Fase: 0°");
        lblFase.setForeground(new Color(210, 225, 240));
        lblFase.setFont(new Font("SansSerif", Font.BOLD, 13));
        lblFase.setPreferredSize(new Dimension(72, 20));

        JSlider sliderFase = new JSlider(0, 360, 0);
        sliderFase.setPreferredSize(new Dimension(140, 24));
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
		
        barraInferiore.add(btnTrigger);
        barraInferiore.add(lblOnda);
        barraInferiore.add(comboOnda);
        barraInferiore.add(lblFase);
        barraInferiore.add(sliderFase);
        barraInferiore.add(btnSync);
        barraInferiore.add(lblCutoff);
        barraInferiore.add(sliderLP);


        // Inizializza i dati grafici all'avvio per mostrare subito lo spettro stereo
        aggiornaDatiSequenza("", 64.0);
		
        // Azione di avvio / aggiornamento
        Runnable avviaOAggiorna = () -> {
            double nuovaBase;
            try {
                nuovaBase = Double.parseDouble(txtFondamentale.getText().trim().replace(",", "."));
                if (nuovaBase < 1 || nuovaBase > 4096.0) {
                    throw new NumberFormatException();
                }
            } catch (NumberFormatException ex) {
                JOptionPane.showMessageDialog(frame,
                        "Inserisci una frequenza fondamentale valida (tra 10 e 2000 Hz).",
                        "Frequenza non valida",
                        JOptionPane.WARNING_MESSAGE);
                return;
            }

            String testo = txtSequenza.getText().replaceAll("[^1-9]", "");
            if (testo.isEmpty()) {
                JOptionPane.showMessageDialog(frame,
                        "Inserisci almeno una cifra valida compresa tra 1 e 9.",
                        "Sequenza non valida",
                        JOptionPane.WARNING_MESSAGE);
                return;
            }

            aggiornaDatiSequenza(testo, nuovaBase);
            lblNotaPitagorica.setText(calcolaNotaPitagorica(nuovaBase));
            pannello.repaint();

            if (!isPlaying) {
                isPlaying = true;
                btnPlayStop.setText("Ferma Suono");
                audioThread = new Thread(NumSound::riproduciOndaSomma);
                audioThread.setPriority(Thread.MAX_PRIORITY);
                audioThread.start();
            }
        };

        // Premendo Invio in uno dei due campi aggiorna subito suono, nota e grafico
        txtSequenza.addActionListener(e -> avviaOAggiorna.run());
        txtFondamentale.addActionListener(e -> avviaOAggiorna.run());

        // Pulsante Avvia / Ferma / Aggiorna
        btnPlayStop.addActionListener(e -> {
            if (isPlaying) {
                String testoAttuale = txtSequenza.getText().replaceAll("[^1-9]", "").replaceAll("(.)", "$1 ").trim();
                double baseInserita = frequenzaFondamentale;
                try {
                    baseInserita = Double.parseDouble(txtFondamentale.getText().trim().replace(",", "."));
                } catch (NumberFormatException ignored) {}

                if (!testoAttuale.equals(sequenzaCorrente) || Math.abs(baseInserita - frequenzaFondamentale) > 0.001) {
                    avviaOAggiorna.run();
                } else {
                    isPlaying = false;
                    btnPlayStop.setText("Avvia Suono");
                }
            } else {
                avviaOAggiorna.run();
        }
    });

        frame.add(barraSuperiore, BorderLayout.NORTH);
        frame.add(pannello, BorderLayout.CENTER);
        frame.add(barraInferiore, BorderLayout.SOUTH);

        frame.setVisible(true);

        Timer timer = new Timer(16, e -> pannello.repaint());
        timer.start();
		// Avvia ascolto automatico da tutte le tastiere MIDI collegate
        inizializzaMidi(txtFondamentale, lblNotaPitagorica, sliderLP, avviaOAggiorna);
    }
    
	// ----- Metodo inizializzaMidi -----
    private static void inizializzaMidi(JTextField txtFondamentale, JLabel lblNotaPitagorica, JSlider sliderLP, Runnable avviaOAggiorna) {
        MidiDevice.Info[] infos = MidiSystem.getMidiDeviceInfo();
        for (MidiDevice.Info info : infos) {
            try {
                MidiDevice device = MidiSystem.getMidiDevice(info);
                if (device.getMaxTransmitters() != 0) {
                    device.open();
                    Transmitter transmitter = device.getTransmitter();
                    transmitter.setReceiver(new Receiver() {
                        @Override
                        public void send(MidiMessage message, long timeStamp) {
                        if (message instanceof ShortMessage) {
                                ShortMessage sm = (ShortMessage) message;
                                if (sm.getCommand() == ShortMessage.NOTE_ON && sm.getData2() > 0) {
                                    int notaMidi = sm.getData1();
                                    int diffDaDoCentrale = notaMidi - 60;
                                    int ottave = Math.floorDiv(diffDaDoCentrale, 12);
                                    int indiceNota = Math.floorMod(diffDaDoCentrale, 12);
                                    double freq = 256.0 * Math.pow(2.0, ottave) * RAPPORTI_PITAGORICI[indiceNota];

                                    String seqAttuale = sequenzaCorrente.replace(" ", "");
                                    if (!seqAttuale.isEmpty()) {
                                        aggiornaDatiSequenza(seqAttuale, freq);
                                    }
                                    
                                    SwingUtilities.invokeLater(() -> {
                                        txtFondamentale.setText(String.format(java.util.Locale.US, "%.2f", freq));
                                        lblNotaPitagorica.setText(calcolaNotaPitagorica(freq)); // <-- AGGIUNGI QUESTA RIGA
                                        if (!isPlaying) {
                                            avviaOAggiorna.run();
                                        }
                                    });
                                }
                                // 2. CONTROL_CHANGE 7 (Volume Master): controlla il Filtro LP
                                else if (sm.getCommand() == ShortMessage.CONTROL_CHANGE && sm.getData1() == 7) {
                                    int midiVal = sm.getData2(); // Valore da 0 a 127
                                    int sliderVal = (int) Math.round((midiVal / 127.0) * 1000.0);
                                    
                                    SwingUtilities.invokeLater(() -> {
                                        // Muovendo lo slider grafico si aggiornano automaticamente sia cutoffLP che l'etichetta Hz!
                                        sliderLP.setValue(sliderVal);
                                    });
                                }
                            }
                        }

                        @Override
                        public void close() {}
                    });
                    System.out.println("Tastiera MIDI collegata: " + info.getName());
                }
            } catch (MidiUnavailableException ignored) {}
        }
    }
    // ----- Fine Metodo InizializzaMidi -----


    private static void aggiornaDatiSequenza(String testoPulito, double nuovaBase) {
        double[] nuoveFreq = new double[testoPulito.length()];
        int[] nuoveCifre = new int[testoPulito.length()];
        StringBuilder seqPulita = new StringBuilder();

        for (int i = 0; i < testoPulito.length(); i++) {
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

    /**
     * Calcola la nota pitagorica più vicina basata su DO2 = 128 Hz (DO1 = 64 Hz)
     */
    private static String calcolaNotaPitagorica(double freq) {
        if (freq <= 0) return "Nota: --";
		double refDo2 = 128.0;
		int ottava = 2;
		double freqNormalizzata = freq;
		while (freqNormalizzata < refDo2) {
			freqNormalizzata *= 2.0;
			ottava--;
		}
		while (freqNormalizzata >= refDo2 * 2.0) {
			freqNormalizzata /= 2.0;
			ottava++;
		}
		double rapportoCorrente = freqNormalizzata / refDo2;
		int indiceMigliore = 0;
		double minDiffCents = Double.MAX_VALUE;
		double freqNotaEsatta = refDo2;
		for (int i = 0; i < RAPPORTI_PITAGORICI.length; i++) {
			double rapportoNota = RAPPORTI_PITAGORICI[i];
			double diffCents = 1200.0 * Math.abs(Math.log(rapportoCorrente / rapportoNota) / Math.log(2.0));
			if (diffCents < minDiffCents) {
				minDiffCents = diffCents;
				indiceMigliore = i;
				freqNotaEsatta = rapportoNota * 128.0 * Math.pow(2.0, ottava - 2);
			}
		}
		double diffDoSuperiore = 1200.0 * Math.abs(Math.log(rapportoCorrente / 2.0) / Math.log(2.0));
		if (diffDoSuperiore < minDiffCents) {
  			minDiffCents = diffDoSuperiore;
  			indiceMigliore = 0;
  			ottava++;
  			freqNotaEsatta = 128.0 * Math.pow(2.0, ottava - 2);
		}
		String nomeNota = NOMI_NOTE[indiceMigliore] + ottava;
		if (minDiffCents < 1.5) {
  			return String.format("Nota: %s (%.1f Hz)", nomeNota, freqNotaEsatta);
		} else {
  			return String.format("Nota: ~%s (%.1f Hz)", nomeNota, freqNotaEsatta);
		}
	}
	
    private static Color calcolaColoreDaFrequenza(double freq) {
        if (freq <= 0) return Color.GRAY;
        // Calcola i semitoni pitagorici rispetto a DO1 (64 Hz)
        double semitoniDaDo1 = 12.0 * (Math.log(freq / 64.0) / Math.log(2.0));
        // Posizione normalizzata nell'ottava [0.0, 1.0) dove 0.0 = DO, 0.583 = SOL, ecc.
        double posOttava = ((semitoniDaDo1 % 12.0) + 12.0) % 12.0 / 12.0;

        // Mappa la nota sulla ruota cromatica partendo dal Verde (0.36) per il DO
        // In questo modo: DO = Verde, RE = Ciano/Blu, MI = Viola, SOL = Rosso, SIb = Arancio
        float hue = (float) ((0.36 + posOttava * 1.12) % 1.0);

        // Luminosità e saturazione variano con l'ottava (più scuro sui bassi, più chiaro sugli acuti)
        double ottava = semitoniDaDo1 / 12.0;
        float brightness = (float) Math.max(0.35, Math.min(1.0, 0.45 + ottava * 0.16));
        float saturation = (float) Math.max(0.45, Math.min(0.95, 0.95 - Math.max(0, ottava - 2.5) * 0.18));

        return Color.getHSBColor(hue, saturation, brightness);
    }	
	
    private static void riproduciOndaSomma() {
        AudioFormat format = new AudioFormat(SAMPLE_RATE, 16, 2, true, false);
        try (SourceDataLine line = AudioSystem.getSourceDataLine(format)) {
            line.open(format, 2048); // 2048 byte = 512 frame = ~11.6 ms (minimo stabile su Java)
            line.start();

            int chunkSize = 64;
            byte[] audioBuffer = new byte[chunkSize * 4];
            double[] rollingHistory = new double[BUFFER_SAMPLES];

            // Accumulatori di fase dedicati (restano sempre tra 0.0 e 1.0 senza mai perdere precisione)
            double[] fasi = new double[0];

            // Fade-in e Fade-out di ~80 ms
            double envelope = 0.0;
            double fadeStep = 1.0 / (SAMPLE_RATE * 0.08);

            // Stati per filtro Passa-Basso a 2 poli (12 dB/ottava) stereo
            double lpLeft1 = 0.0, lpLeft2 = 0.0;
            double lpRight1 = 0.0, lpRight2 = 0.0;

            int contatoreGrafica = 0;

            while (isPlaying || envelope > 0.0) {
                double[] freqArray = frequenzeAttive;
                if (freqArray == null || freqArray.length == 0) {
                    if (!isPlaying) break;
                    continue;
                }
                int numOsci = freqArray.length;

                // Se cambia il numero di cifre nella sequenza, ridimensiona l'array delle fasi mantenendo quelle esistenti
                if (fasi.length != numOsci) {
                    double[] nuoveFasi = new double[numOsci];
                    System.arraycopy(fasi, 0, nuoveFasi, 0, Math.min(fasi.length, numOsci));
                    fasi = nuoveFasi;
                }
                
                if (resetFasiRichiesto) {
                    Arrays.fill(fasi, 0.0);
                    resetFasiRichiesto = false;
                }

                double[] gainLeft = new double[numOsci];
                double[] gainRight = new double[numOsci];

                for (int k = 0; k < numOsci; k++) {
                    double angolo = (numOsci == 1) ? (Math.PI / 4.0) : (k * (Math.PI / 2.0) / (numOsci - 1));
                    gainLeft[k] = Math.cos(angolo);
                    gainRight[k] = Math.sin(angolo);
                }

                System.arraycopy(rollingHistory, chunkSize, rollingHistory, 0, BUFFER_SAMPLES - chunkSize);

                int tipoOndaCorrente = formaOnda;

                // Calcola il coefficiente del filtro LP dalla frequenza di taglio attuale
                double alpha;
                if (cutoffLP <= 0.0) {
                    alpha = 0.0; // Chiude completamente il filtro (silenzio assoluto a 0 Hz)
                } else {
                    double dt = 1.0 / SAMPLE_RATE;
                    double rc = 1.0 / (2.0 * Math.PI * cutoffLP);
                    alpha = dt / (rc + dt);
                }

                for (int i = 0; i < chunkSize; i++) {
                    if (isPlaying && envelope < 1.0) {
                        envelope = Math.min(1.0, envelope + fadeStep);
                    } else if (!isPlaying && envelope > 0.0) {
                        envelope = Math.max(0.0, envelope - fadeStep);
                    }

                    double sommaLeft = 0.0;
                    double sommaRight = 0.0;

                    for (int k = 0; k < numOsci; k++) {
                        double fase = fasi[k];
                        double distCentro = k - (numOsci - 1) / 2.0;
						double faseEffettiva = fase - (distCentro * sfasamentoNorm);
						faseEffettiva -= Math.floor(faseEffettiva);
                        if (faseEffettiva >= 1.0) {
                            faseEffettiva -= Math.floor(faseEffettiva);
                        }

                        double onda;
                        if (tipoOndaCorrente == 1) {
                            onda = 1.0 - 4.0 * Math.abs(faseEffettiva - 0.5);
                        } else if (tipoOndaCorrente == 2) {
                            onda = (faseEffettiva < 0.5) ? 1.0 : -1.0;
                        } else if (tipoOndaCorrente == 3) {
                            onda = (2.0 * faseEffettiva) - 1.0;
                        } else {
                            onda = Math.sin(2.0 * Math.PI * faseEffettiva);
                        }

                        sommaLeft += onda * gainLeft[k];
                        sommaRight += onda * gainRight[k];

                        fase += freqArray[k] / SAMPLE_RATE;
                        if (fase >= 1.0) {
                            fase -= Math.floor(fase);
                        }
                        fasi[k] = fase;
                    }
                    
                    // Applica filtro Passa-Basso a 2 poli (smussa le armoniche alte e le forme d'onda)
                    lpLeft1 += alpha * (sommaLeft - lpLeft1);
                    lpLeft2 += alpha * (lpLeft1 - lpLeft2);
                    sommaLeft = lpLeft2;

                    lpRight1 += alpha * (sommaRight - lpRight1);
                    lpRight2 += alpha * (lpRight1 - lpRight2);
                    sommaRight = lpRight2;

                    // Normalizzazione percettiva (radice quadrata di numOsci) + soft-clipping (tanh)
                    double fattoreNorm = Math.sqrt(numOsci);
                    double normLeft = Math.tanh((sommaLeft / fattoreNorm) * 0.65) * 0.65 * envelope;
                    double normRight = Math.tanh((sommaRight / fattoreNorm) * 0.65) * 0.65 * envelope;

                    rollingHistory[(BUFFER_SAMPLES - chunkSize) + i] = ((normLeft + normRight) * 0.5) * 2.0;

                    short valLeft = (short) (normLeft * Short.MAX_VALUE);
                    short valRight = (short) (normRight * Short.MAX_VALUE);

                    audioBuffer[i * 4] = (byte) (valLeft & 0xFF);
                    audioBuffer[i * 4 + 1] = (byte) ((valLeft >> 8) & 0xFF);

                    audioBuffer[i * 4 + 2] = (byte) (valRight & 0xFF);
                    audioBuffer[i * 4 + 3] = (byte) ((valRight >> 8) & 0xFF);
                }

                if (++contatoreGrafica % 8 == 0) {
                    latestSnapshot = rollingHistory.clone();
                }
                line.write(audioBuffer, 0, audioBuffer.length);
            }

            latestSnapshot = new double[BUFFER_SAMPLES];
            line.drain();
            line.stop();
        } catch (LineUnavailableException e) {
            SwingUtilities.invokeLater(() -> JOptionPane.showMessageDialog(null,
                    "Errore scheda audio: " + e.getMessage(),
                    "Errore Audio",
                    JOptionPane.ERROR_MESSAGE));
            isPlaying = false;
        }
    }

// --- PANNELLO OSCILLOSCOPIO + SPETTRO PANORAMA STEREO ---
private static class PannelloOnda extends JPanel {
  @Override
  protected void paintComponent(Graphics g) {
    super.paintComponent(g);
    Graphics2D g2 = (Graphics2D) g;
    g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
    int w = getWidth();
    int h = getHeight();
    // Suddivide l'altezza: ~65% Oscilloscopio in alto, ~35% Panorama Stereo in basso
    int altezzaSpettro = Math.max(170, (int)(h * 0.35));
    int altezzaOnda = h - altezzaSpettro;
    int midY = altezzaOnda / 2;
    // --- 1. SFONDO E GRIGLIA OSCILLOSCOPIO ---
    //g2.setColor(new Color(16, 20, 26));
    g2.setColor(Color.BLACK);
    g2.fillRect(0, 0, w, altezzaOnda);
    // Griglia sincronizzata con i cicli dell'onda (16 divisioni orizzontali = 8 per ciclo)
    int numDivisioniX = 16;
    for (int i = 1; i < numDivisioniX; i++) {
    	int x = (int) Math.round((i / (double) numDivisioniX) * w);
        // Evidenzia leggermente la linea a metà schermo (fine del 1° ciclo completo)
        g2.setColor(i == 8 ? new Color(48, 64, 82) : new Color(32, 42, 54));
        g2.drawLine(x, 0, x, altezzaOnda);
     }

     // Griglia verticale sincronizzata con l'ampiezza massima (0.42 * altezzaOnda)
     double ampMaxPixel = altezzaOnda * 0.42;
     int numDivisioniY = 4; // 4 tacche sopra lo zero e 4 sotto (ogni tacca = 25% di ampiezza)
     g2.setColor(new Color(32, 42, 54));
     for (int i = 1; i <= numDivisioniY; i++) {
        int offsetY = (int) Math.round((i / (double) numDivisioniY) * ampMaxPixel);
        g2.drawLine(0, midY - offsetY, w, midY - offsetY); // Sopra lo zero
        g2.drawLine(0, midY + offsetY, w, midY + offsetY); // Sotto lo zero
     }

    // Asse centrale zero
    g2.setColor(new Color(60, 80, 100));
    g2.drawLine(0, midY, w, midY);
    double[] samples = latestSnapshot;
    if (samples != null && samples.length > 0) {
      int periodoAttuale = (int) Math.round(SAMPLE_RATE / frequenzaFondamentale);
      int displaySamples = Math.min(BUFFER_SAMPLES - periodoAttuale - 10, Math.max(100, periodoAttuale * 2));
      int startIdx = BUFFER_SAMPLES - displaySamples;
      if (triggerAttivo && isPlaying) {
                    int searchStart = Math.max(1, BUFFER_SAMPLES - displaySamples - periodoAttuale);
                    int searchEnd = Math.min(BUFFER_SAMPLES - displaySamples, searchStart + periodoAttuale);
                    int peakIdx = searchStart;
                    double maxVal = -2.0;
                    for (int i = searchStart; i < searchEnd; i++) {
                        if (samples[i] > maxVal) {
                            maxVal = samples[i];
                            peakIdx = i;
                        }
                    }
                    // Arretra dal picco fino all'attraversamento dello zero in salita
                    int zeroIdx = peakIdx;
                    int minBack = Math.max(1, peakIdx - periodoAttuale);
                    for (int j = peakIdx; j >= minBack; j--) {
                        if (samples[j - 1] < 0.0 && samples[j] >= 0.0) {
                            zeroIdx = j;
                            break;
                        }
                    }
                    startIdx = zeroIdx;
      }
      
      // --- DISEGNO ONDE SINGOLE ARMONICHE ---
      int[] cifre = cifreAttive;
      if (mostraOndeSingole && isPlaying && cifre != null && cifre.length > 0) {
           int numOsci = cifre.length;
           double fattoreNorm = Math.sqrt(numOsci);
           double scalaAmp = (0.65 / fattoreNorm) * (altezzaOnda * 0.42);
           g2.setStroke(new BasicStroke(1.2f));

           for (int k = 0; k < numOsci; k++) {
              int cifra = cifre[k];
              Color c = calcolaColoreDaFrequenza(cifra * frequenzaFondamentale);
              g2.setColor(new Color(c.getRed(), c.getGreen(), c.getBlue(), 130));

              double stepFase = (cifra * frequenzaFondamentale) / SAMPLE_RATE;
              double distCentro = k - (numOsci - 1) / 2.0;
			  double faseOffset = -(distCentro * sfasamentoNorm);

              int pX = 0;
              int pY = midY;
              for (int i = 0; i < displaySamples; i++) {
                  double f = (i * stepFase) + faseOffset;
                  f -= Math.floor(f);

                  double val;
               	  if (formaOnda == 1) val = 1.0 - 4.0 * Math.abs(f - 0.5);
                  else if (formaOnda == 2) val = (f < 0.5) ? 1.0 : -1.0;
                  else if (formaOnda == 3) val = (2.0 * f) - 1.0;
                  else val = Math.sin(2.0 * Math.PI * f);

                  int x = (int) ((i / (double) (displaySamples - 1)) * w);
                  int y = midY - (int) (val * scalaAmp);
                  if (i > 0) g2.drawLine(pX, pY, x, y);
                  pX = x;
                  pY = y;
               }
           }
      }

      
      // g2.setColor(new Color(0, 230, 160));
      // g2.setStroke(new BasicStroke(2.2f));
      // g2.setColor(Color.WHITE);
      Color coloreSomma = Color.WHITE;
      double[] freqPerColore = frequenzeAttive;
      if (freqPerColore != null && freqPerColore.length > 0) {
                 double rTot = 0, gTot = 0, bTot = 0;
                 for (double f : freqPerColore) {
                     Color col = calcolaColoreDaFrequenza(f);
                     rTot += col.getRed();
                     gTot += col.getGreen();
                     bTot += col.getBlue();
       }
       double normCol = Math.sqrt(freqPerColore.length);
       int r = Math.min(255, (int) Math.round(rTot / normCol));
       int gr = Math.min(255, (int) Math.round(gTot / normCol));
       int b = Math.min(255, (int) Math.round(bTot / normCol));
       coloreSomma = new Color(r, gr, b);
      }

      g2.setColor(coloreSomma);   
	  g2.setStroke(new BasicStroke(3.0f));

      
      int prevX = 0;
      int prevY = midY - (int)(samples[startIdx] * (altezzaOnda * 0.42));
      for (int i = 1; i < displaySamples; i++) {
        int x = (int)((i / (double)(displaySamples - 1)) * w);
        int y = midY - (int)(samples[startIdx + i] * (altezzaOnda * 0.42));
        g2.drawLine(prevX, prevY, x, y);
        prevX = x;
        prevY = y;
      }
    }
    // --- 2. GRAFICO A BARRE: SPETTRO & PANORAMA STEREO ---
    int topSpettro = altezzaOnda;
    g2.setColor(new Color(20, 26, 34));
    g2.fillRect(0, topSpettro, w, altezzaSpettro);
    // Linea divisoria tra oscilloscopio e spettro
    g2.setColor(new Color(50, 65, 82));
    g2.setStroke(new BasicStroke(1.5f));
    g2.drawLine(0, topSpettro, w, topSpettro);
    int margineX = 65;
    int areaUtileX = w - (margineX * 2);
    int baseY = h - 38;
    int maxBarHeight = altezzaSpettro - 72;
    // Linea di base e indicatori L - C - R
    g2.setColor(new Color(55, 72, 90));
    g2.drawLine(margineX - 20, baseY, w - margineX + 20, baseY);
    // Linea verticale tratteggiata al Centro (C)
    Stroke defaultStroke = g2.getStroke();
    g2.setStroke(new BasicStroke(1.0f, BasicStroke.CAP_BUTT, BasicStroke.JOIN_BEVEL, 0, new float[] {
      4
    }, 0));
    g2.drawLine(w / 2, topSpettro + 22, w / 2, baseY);
    g2.setStroke(defaultStroke);
    g2.setFont(new Font("SansSerif", Font.BOLD, 11));
    g2.setColor(new Color(140, 165, 190));
    String titoloSpettro = "PANORAMA STEREO & OCCORRENZE ARMONICHE";
	int titoloW = g2.getFontMetrics().stringWidth(titoloSpettro);
	g2.drawString(titoloSpettro, (w - titoloW) / 2, topSpettro + 18);
    g2.setColor(new Color(120, 150, 180));
    g2.drawString("L (100% Left)", margineX - 35, h - 10);
    g2.drawString("C (Center)", (w / 2) - 26, h - 10);
    g2.drawString("R (100% Right)", w - margineX - 45, h - 10);
    int[] cifre = cifreAttive;
    double[] freqs = frequenzeAttive;
    if (cifre == null || cifre.length == 0) return;
    // Conta le occorrenze di ogni cifra (1..9) e trova il massimo per scalare l'altezza
    int[] occorrenze = new int[10];
    int maxOccorrenze = 1;
    for (int c: cifre) {
      if (c >= 1 && c <= 9) {
        occorrenze[c]++;
        if (occorrenze[c] > maxOccorrenze) {
          maxOccorrenze = occorrenze[c];
        }
      }
    }
    int numOsci = cifre.length;
    // Larghezza dinamica delle barre in base a quante cifre ci sono
    int barWidth = Math.max(10, Math.min(38, (areaUtileX / Math.max(1, numOsci)) - 10));
    for (int k = 0; k < numOsci; k++) {
      int cifra = cifre[k];
      int count = occorrenze[cifra];
      double freq = freqs[k];
      // Posizione orizzontale proporzionale al panning (0.0 = Left, 0.5 = Center, 1.0 = Right)
      double normPan = (numOsci == 1) ? 0.5 : (k / (double)(numOsci - 1));
      int centerX = margineX + (int) Math.round(normPan * areaUtileX);
      int barX = centerX - (barWidth / 2);
      int barH = (int) Math.round((count / (double) maxOccorrenze) * maxBarHeight);
      int barY = baseY - barH;
      // Colore fisico associato alla nota/cifra
      Color barColor = calcolaColoreDaFrequenza(freq);
      // Riempimento barra
      g2.setColor(new Color(barColor.getRed(), barColor.getGreen(), barColor.getBlue(), 190));
      g2.fillRoundRect(barX, barY, barWidth, barH, 6, 6);
      // Bordo barra
      g2.setColor(barColor);
      g2.setStroke(new BasicStroke(1.5f));
      g2.drawRoundRect(barX, barY, barWidth, barH, 6, 6);
      // Cifra subito sotto la barra
      g2.setFont(new Font("Monospaced", Font.BOLD, 14));
      g2.setColor(Color.WHITE);
      String lblCifra = String.valueOf(cifra);
      int strW = g2.getFontMetrics().stringWidth(lblCifra);
      g2.drawString(lblCifra, centerX - (strW / 2), baseY + 15);
      // Frequenza e occorrenze sopra la barra (se c'è abbastanza spazio orizzontale)
      if (numOsci <= 22) {
        g2.setFont(new Font("SansSerif", Font.PLAIN, 10));
        g2.setColor(new Color(195, 215, 235));
        String infoTop = (count > 1) ?
          String.format("%.0fHz (x%d)", freq, count) :
          String.format("%.0fHz", freq);
        int infoW = g2.getFontMetrics().stringWidth(infoTop);
        g2.drawString(infoTop, centerX - (infoW / 2), barY - 5);
      }
    }
  }
}
}