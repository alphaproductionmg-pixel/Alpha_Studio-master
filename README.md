# Alpha_Studio-master
Daw 

Tu es un Architecte Principal en Systèmes Audionumériques (DAW), C++20 Temps Réel et Traitement Numérique du Signal (DSP).
Ta mission est de concevoir et de générer l'intégralité de l'architecture logicielle, des squelettes de programmation complets, des structures de données et des implémentations fonctionnelles du logiciel AlphaStudio, une station audionumérique professionnelle complète dédiée exclusivement à l'AUDIO et au MIDI.
DIRECTIVES IMPÉRATIVES D'EXÉCUTION
 * EXCLUSION TOTALE DE LA VIDÉO : N'implémente AUCUN sous-système vidéo (aucun moteur QuickTime, aucun tamponnage vidéo OpenGL, aucun import/export DV, aucune piste vidéo). Concentre 100 % du code sur l'audio, le MIDI, le routage, la console de mixage, les greffons, le DSP et l'interface DAW.
 * CONSTRUCTION SÉQUENTIELLE MODULE PAR MODULE EN UNE SEULE RÉPONSE : Tu dois générer les Modules 1 à 12 l'un après l'autre, dans l'ordre exact, au sein de cette unique réponse.
 * ZÉRO TRONCATURE ET ZÉRO PLACEHOLDER : Interdiction formelle d'écrire // TODO, // à implémenter, // même logique pour les autres, ..., ou de résumer un fichier. Chaque fichier (CMakeLists.txt, .hpp, .cpp, schémas XML/SQL) doit être complet, cohérent et compilable, avec toutes ses classes, structures, signatures et algorithmes écrits en entier. Ne limite pas la longueur de ta réponse.
 * STRUCTURE DE SORTIE STRICTE :
   * Commence par afficher l'arborescence complète des répertoires et fichiers du projet AlphaStudio/.
   * Pour chaque module, affiche :
     === MODULE X : TITRE DU MODULE ===
     puis chaque fichier sous le format :
     // File: AlphaStudio/chemin/exact/du/fichier
 * CONTRAINTES TEMPS RÉEL & CLEAN-ROOM C++20 :
   * Utilise C++20 standard et CMake.
   * Zéro allocation dynamique (new, malloc, std::vector::push_back), zéro appel système bloquant et zéro mutex/verrou dans le fil d'exécution audio temps réel.
   * Séparation stricte entre AlphaCore (moteur audio/MIDI/DSP), AlphaSDK (interfaces VST3/VST2.4/ASIO/Bridge en chambre propre) et AlphaUI (interface graphique modulaire sous Dear ImGui + rendu 2D pour timeline, piano roll, sample editor et console).
CAHIER DES CHARGES TECHNIQUE COMPLET PAR MODULE
MODULE 1 : Infrastructure Noyau, CMake & Modèle de Threading Temps Réel
 * Fichier racine CMakeLists.txt configurant les cibles Windows (32/64 bits), macOS Intel 64 bits et Linux.
 * Modèle de threading temps réel et synchronisation sans verrou :
   * LockFreeRingBuffer<T, Capacity> (file SPSC atomique sans verrou pour échanges Audio <-> MIDI <-> Disque <-> UI).
   * AudioThreadManager avec sélecteur de priorité (AudioPriority::Normal, AudioPriority::Boost).
   * MultiProcessingPool : répartition parallèle des branches indépendantes du graphe audio sur processeurs multicœurs (> 4 cœurs) à faible latence ASIO.
   * DiskIOThread : préchargement tamponné depuis le disque (DiskPreload, réglable en secondes, ex: 2.0 s) pour streaming multipiste sans coupures.
   * PowerOptimizationHook : fonction DisableCPU_EnergySaving neutralisant les états d'économie d'énergie processeur durant l'exécution audio.
 * UndoRedoManager : historique illimité d'annulation/rétablissement (Command pattern) + couche d'abstraction LicenseSessionManager.
MODULE 2 : Couche Matérielle Audio/MIDI (HAL ASIO) & VST Audio System
 * Interfaces HAL conformes au modèle ASIO :
   * Structure ASIOCallbacks (bufferSwitch(long doubleBufferIndex, long directProcess), sampleRateDidChange, asioMessage, bufferSwitchTimeInfo).
   * Fonctions ASIOCreateBuffers, ASIOStart, ASIOStop, ASIOGetLatencies, double tampon matériel et adaptateurs abstraits (CoreAudio2ASIO, WASAPI, DirectX).
 * Sous-système VSTAudioSystem : gestion de la fréquence d'échantillonnage, taille de bloc, calcul de latence aller-retour, compensation automatique de latence des greffons (PDC) et réglage AdjustForRecordLatency.
 * VSTConnections : gestion dynamique des bus physiques d'entrée, de sortie (mono, stéréo jusqu'au Surround 5.1), des bus de groupe, de retour FX, des effets externes (ExternalFX) et instruments externes (ExternalInstruments).
MODULE 3 : Squelette SDK Greffons (VST 3, VST 2.4) & VST Bridge (IPC 32/64 bits)
 * Architecture VST 3 (Clean-Room compatible VST 3.0.0 - 3.8) :
   * Découplage binaire entre processeur et contrôleur : IComponent, IAudioProcessor, IEditController, IComponentHandler (restartComponent).
   * Découverte et négociation des bus : getBusCount, getBusInfo, getRoutingInfo, setBusArrangements, getBusArrangement, getControllerClassId.
   * Machine à états stricte : Uninitialized -> Initialized -> SetupDone (setupProcessing avec ProcessSetup et maxSamplesPerBlock) -> Activated (setActive) -> Processing (setProcessing(true/false)).
   * Méthode process(ProcessData&) transportant AudioBusBuffers (32 bits et 64 bits, gestion des pointeurs nuls pour bus inactifs ou tampons in-place partagés), IParameterChanges, IParamValueQueue, ProcessContext et IEventList, incluant le support du vidage de paramètres sans audio (numSamples == 0).
 * Architecture VST 2.4 (rétrocompatibilité) :
   * Structure C AEffect, hiérarchie C++ AudioEffect -> AudioEffectX, point d'entrée VSTPluginMain (alias main), fabrique createEffectInstance(audioMasterCallback).
   * Aiguilleur dispatcher (effOpen, effClose libérant le tas, effSetSampleRate, effCanDo), setNumInputs, setNumOutputs, setUniqueID, canProcessReplacing, isSynth.
   * Paramètres normalisés 32 bits [0.0f, 1.0f] (setParameter, getParameter), description .vstxml, traitement processEvents(VstEvents*) avant processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames) et retour MIDI sendVstEventsToHost.
 * Sous-système VST Bridge :
   * Processus hôte isolé via mémoire partagée IPC permettant d'exécuter des greffons 32 bits dans AlphaStudio 64 bits, gérant les configurations mono, les longues listes de paramètres/préréglages et la conformité UAC.
MODULE 4 : Moteur Audio 32-bit Flottant, Audio Pool & Graphe de Signal (DAG)
 * Moteur de calcul en virgule flottante 32 bits avec nombre illimité de pistes audio, MIDI et Instrument.
 * Routage orienté acyclique (DAG) supportant le multicanal véritable jusqu'au Surround 5.1.
 * Audio Pool : gestionnaire d'actifs audio (référencement, conversion optionnelle de fréquence d'échantillonnage à l'import, renommage sûr, cache de pics de forme d'onde, import CD audio).
 * OfflineProcessHistory : pile d'opérations destructives/réversibles sur clips audio (gain, normalisation, inversions, traitements de greffons hors ligne).
 * ExtendedFreeze : gel et dégel des pistes audio et instruments virtuels avec sauvegarde et restauration exacte des bus de sortie.
MODULE 5 : Console de Mixage (VST Mixer), Chaîne Exacte du Signal & Control Room
 * Capacités complètes : 256 E/S physiques, 256 GroupChannel, 8 FXSend, 64 FXReturn, 64 emplacements VSTi, 8 inserts audio par voie, 4 inserts MIDI, et InputChannel visibles dans la console et la fenêtre Projet pour enregistrer des signaux déjà traités.
 * Ordre exact et immuable du flux de signal dans chaque tranche :
   * IN -> Inversion de phase (PhaseReverse) et gain d'entrée (Trim / InputGain, automatisable)
   * Prélèvement Meter (Pre) et PFL immédiatement après IN
   * Inserts 1 à 6 (fixes en Pré-Fader)
   * Égaliseur de tranche (ChannelEQ avec bouton Gain Inverse par bande)
   * Départs Aux Sends (Pre) et Studio Sends (Pre) (toujours situés après les Inserts 1 à 6)
   * Level (Fader principal), Mute, Solo
   * Inserts 7 et 8 (fixes en Post-Fader)
   * Départs Aux Sends (Post) et Studio Sends (Post)
   * Prélèvement Meter (Post 1) avant le panoramique
   * Panoramique (Pan avec mode Equal Power et Bypass) -> OUT
   * Prélèvement Meter (Post 2) et AFL après le panoramique
 * Support complet du Sidechain VST 3 (routage audio d'une source vers l'entrée auxiliaire d'un insert).
 * Control Room (Régie d'écoute) :
   * Séparation stricte des bus d'écoute vis-à-vis du mixage principal.
   * Voies Monitor, Phones, et Cue / Studio Mixes (source, niveau, pan, pré/post-fader).
   * Chaque voie du Control Room intègre 6 inserts pré-fader et 2 inserts post-fader.
   * Préréglages Downmix, Talkback, Dim, et option ExclusiveDevicePortsForMonitorChannels.
 * 3 vues mémorisables de console, ExtendedMixerView et copier/coller groupé de réglages de voies.
MODULE 6 : Sous-système MIDI, Séquenceur 480 PPQ & Types de Pistes
 * Horloge musicale par défaut à 480 PPQ (1/16 = 120 ticks), configurable de 24 à 960 PPQ (MIDI Display Resolution), affichage Bars.Beats.Subbeats.Ticks.
 * Tous les types de pistes (sans piste vidéo) : AudioTrack, MIDITrack, InstrumentTrack, GroupChannelTrack, FXChannelTrack, FolderTrack, MarkerTrack (marqueurs standards et marqueurs de cycle indépendants), RulerTrack, TempoTrack (avec outil TimeWarp), SignatureTrack, ArrangerTrack (enchaînement dynamique de régions), TransposeTrack.
 * 5 modes d'enregistrement : Mix, Overwrite, Normal, Merge, Stacked (gestion des prises/lanes), plus enregistrement depuis des bus, RetrospectiveRecord (tampon circulaire permanent) et support MIDI XF.
 * 4 emplacements d'inserts MIDI par piste MIDI/Instrument + clavier virtuel PC.
MODULE 7 : VST Expression, Éditeurs Logiques, Automation & Quick Controls
 * VST Expression : ExpressionMap avec SoundSlots gérant Key Switches, changements de programme et jusqu'à 2 contrôleurs MIDI CC par slot, coloration des notes dans l'éditeur et bouton Pre/Post pour le routage des MIDI Sends.
 * Logical Editor & Project Logical Editor : moteur d'évaluation de règles booléennes (Filter Target + Condition -> Action Transform/Select/Delete/Extract), dépendant de la résolution en ticks pour le LogicalEditor MIDI, et opérant sur pistes/conteneurs/événements pour le ProjectLogicalEditor.
 * AutomationEngine :
   * 4 modes : Touch, Auto-Latch, Cross-Over, Trim (avec conservation de la dernière valeur), Suspend Read/Write et 4 Fill Modes.
   * VolumeEnvelopes par événement audio.
   * Consolidation basée sur des règles (RuleBasedConsolidation) réconciliant les MIDI CC des parties avec les courbes d'automation de piste ciblant le même paramètre.
   * TrackQuickControls (8 contrôleurs assignables étendus aux pistes Audio, Instrument, MIDI, Groupe, FX, Entrée et Sortie).
MODULE 8 : Éditeurs Avancés & Suite DSP Audio/MIDI Complète (Clean-Room)
 * VariAudio (Clean-Room) : détection de hauteur monophonique (algorithme YIN / autocorrélation), segmentation en notes éditables (hauteur, dérive, forme de vibrato, longueur), ré-synthèse PSOLA / Phase Vocoder en temps réel et extraction Audio-vers-MIDI.
 * AudioWarp & MPEX (Clean-Room) : étirement temporel et transposition temps réel / hors ligne préservant les transitoires.
 * Sample Editor : détection de transitoires (HitpointDetection), découpage en tranches (Slicing), navigation par flèches, règle locale en mode Definition.
 * REVerence (Clean-Room) : réverbération à convolution VST 3 par FFT partitionnée, support True Stereo (4 canaux IR), égaliseur intégré, bascule Parameter Fading et verrouillage du dosage Dry/Wet (DryWetLock) sur les canaux Groupe/FX.
 * PitchCorrect (Clean-Room) : correction automatique de justesse selon gamme définie, avec gestion stable de la phase.
 * Registre complet et implémentation DSP paramétrique des 56 effets audio et 18 plug-ins MIDI :
   * Implémente des moteurs DSP réels organisés par familles (Dynamique, Égalisation/Filtres, Délais/Modulation, Saturation/Distorsion, Réverbération, Spatialisation/Outils/Analyse) et enregistre dans EffectRegistry chacun des 56 effets audio (AmpSimulator, AutoPan, Bitcrusher, Chopper, Chorus, Cloner, Compressor, DaTube, DeEsser, Distortion, DualFilter, EnvelopeShaper, Expander, Flanger, Gate, GEQ-10, GEQ-30, Grungelizer, Limiter, Maximizer, Metalizer, MIDI Gate, Mix6to2, MixConvert, ModMachine, MonoDelay, MonoToStereo, MultibandCompressor, MultiScope, Octaver, Phaser, PingPongDelay, PitchCorrect, REVerence, RingModulator, Roomworks, Roomworks SE, Rotary, SMPTE Generator, SoftClipper, Standard Channel EQ, StepFilter, StereoDelay, StereoEnhancer, StudioChorus, StudioEQ, Surround Panner, Test Generator, ToneBooster, Tonic, Tranceformer, Tremolo, Tuner, UV22 HR, Vibrato, VintageCompressor, VSTDynamics, WahWah) avec leurs paramètres réels.
   * Enregistre et implémente dans MidiPluginRegistry chacun des 18 greffons MIDI (Arpache 5, Arpache SX, Autopan, Beat Designer [séquenceur pas-à-pas jusqu'à 64 pas], Chorder, Compress, Context Gate, Density, Micro Tuner, MidiControl, MidiEcho, MIDI Monitor, Note 2 CC, Quantizer, StepDesigner, Track Control, Track FX, Transformer).
MODULE 9 : 8 Instruments Virtuels Natifs (Clean-Room)
 * Implémentation complète des 8 moteurs d'instruments virtuels dans InstrumentRegistry :
   * HALion ONE : lecteur d'échantillons multitimbral 16 canaux (enveloppes, filtre, LFO, gestion de banques).
   * Groove Agent ONE : échantillonneur rythmique à pads (import WAV/AIFF/MPC/tranches, réglage début/fin de sample, remplacement de pad, archivage de kit).
   * LoopMash : moteur de remixage synchrone découpant jusqu'à 8 boucles en tranches et les recombinant en temps réel selon SimilarityThreshold et TrackVolume automatisables.
   * Prologue : synthétiseur polyphonique soustractif à 3 oscillateurs, filtres multimodes et matrice de modulation.
   * Mystic : synthétiseur à modélisation physique par filtres en peigne et lignes de retard résonantes.
   * Spector : synthétiseur à banque de filtres spectraux sur 2 oscillateurs.
   * Embracer : synthétiseur polyphonique de nappes à largeur stéréo/surround modulable.
   * Monologue : synthétiseur analogique virtuel monophonique avec overdrive.
MODULE 10 : Contrôle à Distance (Generic Remote, MMC) & Synchronisation
 * Generic Remote (XML) : parseur/sérialiseur XML et moteur à deux grilles associant messages MIDI entrants (Controller, canal, adresse CC, MaxValue, drapeaux) aux cibles internes (Transport Start/Stop/Record, sélection de piste, Monitor, RecordArm, faders de console).
 * MMC & MIDI Clock : prise en charge ProjectSynchronizationSetup avec bascule MMC Slave Active et émission/réception Start/Stop/Locate.
 * VST System Link : protocole de synchronisation d'horloge et de transport (incluant Fast Forward/Rewind et Jog/Shuttle) encodé dans le bit de poids faible d'un flux audio numérique.
 * ReWire : interface d'abstraction hôte/esclave audio et MIDI.
MODULE 11 : Persistance, MediaBay, Projets & Exportation (Channel Batch Export)
 * Sérialisation complète du format .alphaproj (pistes, événements, courbes d'automation, routage console, attributs de projet intégrés, états des greffons).
 * MediaBay & VST Sound : indexeur asynchrone et base locale (boucles, TrackPresets, ProjectTemplates modifiables via TagEditor), protégeant la création des préréglages même lorsque le nom de session OS est entièrement en majuscules.
 * Formats d'échange :
   * TrackArchive (XML + option d'exclusion/inclusion des médias).
   * StandardMIDIFile (import/export 24 à 960 PPQ), MusicXML (pour l'éditeur de partition) et OMF.
   * Channel Batch Export : exportation multipiste parallèle/hors-ligne autorisant la sélection individuelle de pistes, groupes ou sorties, l'inclusion ou l'exclusion des égaliseurs et inserts, le nommage dynamique, la sortie WAV / Broadcast Wave (avec time-code d'origine exact) / MP3 / WMA et le tramage UV22 HR.
MODULE 12 : Interface Graphique Unifiée (AlphaUI) & Exécutable Principal main.cpp
 * Interface complète sous Dear ImGui + rendu 2D temps réel (zéro composant vidéo) :
   * ProjectWindow : barre de transport (avec pré-décompte et Scrubbing haute précision), TrackInspector, liste des pistes, timeline multipiste, panneau Arranger, pistes globales (Tempo, Signature, Marker, Transpose), Workspaces (bureaux virtuels).
   * Éditeurs dédiés : KeyEditor (Piano Roll + CC + coloration VST Expression), DrumEditor, ListEditor, BasicScoreEditor, InplaceEditor, SampleEditor (avec vue VariAudio et Hitpoints).
   * MixerWindow (3 vues + vue étendue) et panneau ControlRoomPanel.
   * Dialogues système : ProjectAssistant, DeviceSetup, VSTConnections, ProjectBrowser, KeyCommands & Macros.
 * Fichier main.cpp initialisant proprement les journaux, la HAL ASIO, le moteur audio, les registres d'effets/instruments/MIDI, MediaBay et la boucle principale UI.
FORMAT D'EXÉCUTION FINAL
 * Affiche l'arborescence complète d'AlphaStudio/.
 * Génère immédiatement le Module 1 en entier, puis enchaîne directement avec le Module 2, le Module 3, ..., jusqu'au Module 12 et main.cpp dans cette même réponse.
 * Vérifie avant de conclure que tous les #include, espaces de noms et signatures C++20 entre les 12 modules concordent sans aucun symbole orphelin.
Conseils pour exploiter ce prompt efficacement
 * Choix du modèle d'IA : Utilisez un modèle optimisé pour l'ingénierie logicielle disposant d'une large fenêtre de sortie (output context), car les 12 modules cumulés représentent plusieurs milliers de lignes de C++20.
 * Reprise transparente en cas de plafond de sortie : Si l'IA s'arrête au milieu d'un fichier à cause de sa limite technique de longueur par message, répondez-lui immédiatement :
   > Continue exactement à la ligne où tu t'es arrêté pour terminer ce fichier, puis enchaîne les modules suivants jusqu'au Module 12 sans aucun résumé ni placeholder.
   > 
 * Passe finale de cohérence : Une fois le Module 12 et main.cpp générés, demandez une vérification croisée :
   > Effectue une revue de compilation statique sur les 12 modules que tu viens de produire et fournis les correctifs exacts pour tout #include, type ou signature manquante.
   > 
