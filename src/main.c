/**
  * @file main.c
  *
  * @brief Funzione main, che gestisce i parametri passati dall'utente,
  * legge i dati dai file, inizializza le aree di memoria condivisa e
  * sincronizza i processi figli per poter moltiplicare le matrici A e B
  * (dello stesso ordine) e fare la somma della matrice C risultante.
  *
  *
  * @author Filippo Nevi VR398146
  *
  */

#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#include "utilities.h"
#include "child.h"

#define NUM_PARAMETRI 5+1	// Argomenti (5) + nome eseguibile (1)

int main(int argc, char *argv[]) {

int ** matA, ** matB, ** matC, ordine, numFigli;	// Parametri del programma
int ** matProcessi;									// Matrice per la gestione dei processi
int fileA, fileB, fileC;							// Descriptor dei file delle matrici
int chiusuraFlag = 0;								// Flag per tener traccia di errori riscontrati
int memA, memB, memC, memSomma;						// Riferimenti alle aree di memoria condivise
int i, j, k;
int * matCondA, * matCondB, * matCondC, *sommaCond;	// Dati condivisi dai processi
int semaforo;										// Semaforo che gestisce la memoria condivisa
struct sembuf op;									// Operazione da applicare al semaforo
int *pipeComandi;									// Matrice di pipe per comunicare ai figli i comandi da svolgere
int *pidFigli;										// Array di pid dei figli
int codaMessaggi;
int pid;
char * buffComando;
messaggio msgFiglio;
int processiLiberi;
int messaggiRicevuti;
int processiAttivi;
int indicePipe;


	if(argc != NUM_PARAMETRI) {
		segnala("Errore: i parametri devono essere matriceA matriceB matriceC ordine processiFiglio.");
		chiusuraFlag = 1;
	}

	if((fileMatA = open(argv[1], O_RDONLY, S_IRUSR)) < 0) {	// Apertura file della matrice A
		segnala("Errore: impossibile aprire il file della matrice A.");
		chiusuraFlag = 1;
	}

	if((fileMatB = open(argv[2], O_RDONLY, S_IRUSR)) < 0) {	// Apertura file della matrice B
		segnala("Errore: impossibile aprire il file della matrice B.");
		chiusuraFlag = 1;
	}

	if((fileMatC = creat(argv[3], O_RDWR | O_TRUNC | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
		segnala("Errore: impossibile creare il file della matrice C.");
		chiusuraFlag = 1;
	}

	if((ordine = atoi(argv[4])) <= 0) {
		segnala("Errore: l'ordine delle matrici deve essere maggiore di zero.");
		chiusuraFlag = 1;
	}

	if((numFigli = atoi(argv[5])) <= 0) {
		segnala("Errore: il numero dei processi figli deve essere maggiore di zero.");
		chiusuraFlag = 1;
	}

	if(chiusuraFlag) {
		close(fileMatA);
		close(fileMatB);
		close(fileMatC);

		return 0;
	}

	pipeComandi = malloc(int * sizeof(numFigli));
	if(pipeComandi == NULL) {
		segnala("Errore: impossibile creare la matrice di pipe.");
		return 0;
	}

	matA = crea_matrice(ordine);
	matB = crea_matrice(ordine);
	matC = crea_matrice(ordine);

	matProcessi = crea_matrice(ordine);

	leggiMatrice(fileMatA, matA);
	leggiMatrice(fileMatB, matB);

	/// Creazione delle quattro aree di memoria condivisa
	memA = shmget(SHM_KEY_A, sizeof(int[ordine][ordine]), (0666 | IPC_CREAT));	// IPC_CREAT crea una nuova entry se key non esiste
	memB = shmget(SHM_KEY_B, sizeof(int[ordine][ordine]), (0666 | IPC_CREAT));
	memC = shmget(SHM_KEY_C, sizeof(int[ordine][ordine]), (0666 | IPC_CREAT));
	memSomma = shmget(SHM_KEY_SOMMA, sizeof(int), (0666 | IPC_CREAT));

	if((memA == -1) || (memB == -1) || (memC == -1) || (memSomma == -1)) {
		segnala("Errore: creazione della memoria condivisa non riuscita.");
		free(pipeComandi);
		return 0;
	}

	matCondA = shmat(memA, NULL, 0);
	if(matCondA == (void *)-1) {
		segnala("Errore: impossibile effettuare attach della matrice A.");
		free(pipeComandi);
		return 0;
	}

	matCondB = shmat(memB, NULL, 0);
	if(matCondB == (void *)-1) {
		segnala("Errore: impossibile effettuare attach della matrice B.");
		free(pipeComandi);
		return 0;
	}

	matCondC = shmat(memC, NULL, 0);
	if(matCondC == (void *)-1) {
		segnala("Errore: impossibile effettuare attach della matrice C.");
		free(pipeComandi);
		return 0;
	}

	sommaCond = shmat(memSomma, NULL, 0);
	if(sommaCond == (void *)-1) {
		segnala("Errore: impossibile effettuare attach della somma di C.");
		free(pipeComandi);
		return 0;
	}

	*sommaCond = 0;

	semaforo = semget(SEM_KEY, 1, 0666 | IPC_CREAT | IPC_EXCL);
	if(semaforo < 0) {
		segnala("Errore: impossibile creare il semaforo.");
		free(pipeComandi);
		return 0;
	}

	// sem_num = 0 perché c'è solo un semaforo, e lo setto a 1 in modo da usarlo come mutex
	op.sem_num = 0;
	op.sem_op = 1;
	op.sem_flg = 0;

	// Applico op a semaforo (1 = numero di operazioni da applicare)
	semop(semaforo, &op, 1);

	pidFigli = malloc(numFigli * sizeof(int));
	if(pidFigli == NULL) {
		segnala("Errore: impossibile creare i processi figlio.");
		free(pipeComandi);
		return 0;
	}

	codaMessaggi = msgget(MSG_KEY, 0666 | IPC_CREAT | IPC_EXCL);
	if(codaMessaggi == -1) {
		segnala("Errore: impossibile creare la coda dei messaggi.");
		free(pipeComandi);
		free(pidFigli);
		return 0;

	/// Creazione figli
	for(i = 0; i < numFigli; i++) {
		/// Instauro la pipe
		if(pipe(pipeComandi[i]) == -1) {
			segnala("Errore: impossibile creare la pipe.");
			free(pidFigli);
			free(pipeComandi);
			free matA
			free matB
			free matC
			free somma
			return 0;
		}

		/// Creo i figli
		pid = fork();
		if(pid == -1) {
			segnala("Errore: impossibile creare il processo figlio.");
			free(pidFigli);
			free(pipeComandi);
			return 0;
		}
		else if(pid == 0) {				// Codice processo figlio
			if(close(pipeComandi[i][1]) == -1) {	// Chiudo la pipe di scrittura del figlio
				segnala("Errore: impossibile chiudere la pipe di scrittura nel child.");
				free(pidFigli);
				free(pipeComandi);
				exit(1);
			}

			leggiComando(pipeComandi[i][0]);

			if(close(pipeComandi[i][0]) == -1) {	// Chiudo la pipe di scrittura del figlio
				segnala("Errore: impossibile chiudere la pipe di lettura nel child.");
				free(pidFigli);
				free(pipeComandi);
				exit(1);
			}

			exit(0);
		}
		else {							// Codice padre
			pidFigli[i] = pid;
			close(pipeComandi[i][0]);
		}
	}

	if((buffComando = malloc(sizeof(char) * DIM_COM)) == NULL) {
		segnala("Errore: impossibile allocare il buffer del comando.");
		free(pidFigli);
		free(pipeComandi);
		return 0;
	}

	if(ordine == 1) {
		// Creo il comando e lo invio al figlio
		creaComando(buffComando, MOLTIPLICA, 0, 0, ordine);
		write(pipeComandi[0][1], buffComando, strlen(buffComando));

		// Mi metto in ascolto della risposta del figlio
		msgrcv(codaMessaggi, &msgFiglio, sizeof(messaggio) - sizeof(long), 1, 0);

		// Segno che l'unica cella da elaborare è stata completata
		matProcessi[msgFiglio.riga][msgFiglio.colonna] = 1;
	}
	else if((ordine * ordine) <= numFigli) {

		// Faccio partire le moltiplicazioni
		for(i = 0; i < ordine; i++)
			for(j = 0; j < ordine; j++) {
				creaComando(buffComando, MOLTIPLICA, i, j, ordine);
				write(pipeComandi[(i * ordine) + j][1], buffComando, strlen(buffComando));
			}

		
		// Faccio partire le somme
		processiLiberi = numFigli - (ordine * ordine);
		if(ordine <= processiLiberi) {
			k = 0;
			for(i = ordine * ordine; i < (ordine * ordine) + ordine - 1; i++, k++) {
				creaComando(buffComando, SOMMA, k, -1, ordine);
				write(pipeComandi[i][1], buffComando, strlen(buffComando));
			}

			processiAttivi = (ordine * ordine) + ordine;
			// Ricevo i messaggi dai figli a cui ho assegnato un'operazione
			for(i = 0; i < processiAttivi; i++) {
				// Mi metto in ascolto della risposta di un figlio
				msgrcv(codaMessaggi, &msgFiglio, sizeof(messaggio) - sizeof(long), 1, 0);

				if(msgFiglio.comando == MOLTIPLICA) {
					// Segno che la cella assegnata è stata completata
					matProcessi[msgFiglio.riga][msgFiglio.colonna] = 1;
				}
			}
		}
		else {
			k = 0;
			for(i = ordine * ordine; i < ordine + processiLiberi; i++, k++) {
				creaComando(buffComando, SOMMA, k, -1, ordine);
				write(pipeComandi[i][1], buffComando, strlen(buffComando));
			}

			int righeDaSommare = ordine - processiLiberi;
			trovato = 0;
			i = messaggiRicevuti = 0;
			while(i < righeDaSommare) {
				msgrcv(codaMessaggi, &msgFiglio, sizeof(messaggio) - sizeof(long), 1, 0);

				// Cerco il pid del figlio che ha terminato
				for(j = 0; j < numFigli && !trovato; j++) {
					if(pidFigli[j] == msgFiglio.pid) {
						indicePipe = j;
						trovato = 1;
					}
				}

				creaComando(buffComando, SOMMA, (ordine * ordine) + processiLiberi + i, -1, ordine);
				write(pipeComandi[indicePipe][1], buffComando, strlen(buffComando));

				messaggiRicevuti++;
				i++;
			}

			while(messaggiRicevuti < (ordine * ordine) + ordine) {
				msgrcv(codaMessaggi, &msgFiglio, sizeof(messaggio) - sizeof(long));

				if(msgFiglio.comando == MOLTIPLICA) {
					// Segno che la cella assegnata è stata completata
					matProcessi[msgFiglio.riga][msgFiglio.colonna] = 1;
				}
				
				messaggiRicevuti++;
			}
		}
	}
	else {
		int operazioniTotali = (ordine * ordine) + ordine;			// Numero di operazioni totali da compiere
		int count = 0, operazioniEffettuate, prossimaRiga, prossimaColonna;
		int finito = 0;												// Flag che indica se sono stati fatti partire tutti i processi possibili

		for(i = 0; i < ordine && !finito; i++) {
			for(j = 0; j < ordine && !finito; j++) {
				if(count <= numFigli) {
					creaComando(buffComando, MOLTIPLICA, i, j, ordine);
					write(pipeComandi[(i * ordine) + j][1]);
					count++;
				}
				else {
					finito = 1;
					prossimaRiga = i + 1;
					prossimaColonna = j + 1;
				}
			}
		}

		operazioniEffettuate = numFigli;

		// Procedo con le restanti moltiplicazioni
		while(operazioniEffettuate <= (ordine * ordine)) {
			msgrcv(codaMessaggi, &msgFiglio, sizeof(messaggio) - sizeof(long), 1, 0);

			// Cerco il pid del figlio che ha terminato
			for(j = 0; j < numFigli && !trovato; j++) {
				if(pidFigli[j] == msgFiglio.pid) {
					indicePipe = j;
					trovato = 1;
				}
			}

			creaComando(buffComando, MOLTIPLICA, prossimaRiga, prossimaColonna, ordine);
			write(pipeComandi[indicePipe][1], buffComando, strlen(buffComando));

			prossimaRiga++;
			prossimaColonna++;
			operazioniEffettuate++;
		}

		i = 0;								// Indice della riga da sommare

		// Effettuo le somme
		while(operazioniEffettuate <= operazioniTotali) {
			msgrcv(codaMessaggi, &msgFiglio, sizeof(messaggio) - sizeof(long), 1, 0);

			// Cerco il pid del figlio che ha terminato
			for(j = 0; j < numFigli && !trovato; j++) {
				if(pidFigli[j] == msgFiglio.pid) {
					indicePipe = j;
					trovato = 1;
				}
			}

			creaComando(buffComando, SOMMA, i, -1, ordine);
			write(pipeComandi[indicePipe][1], buffComando, strlen(buffComando));

			i++;
			operazioniEffettuate++;
		}
	}

	
}