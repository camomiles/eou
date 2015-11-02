// TODO Include licence header back
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#define EOU_PORT	3301

void	eouattach(int);
int	eouioctl(struct ifnet *, u_long, caddr_t);
void	eoustart(struct ifnet *);
int	eou_clone_create(struct if_clone *, int);
int	eou_clone_destroy(struct ifnet *);
int	eou_media_change(struct ifnet *);
void	eou_media_status(struct ifnet *, struct ifmediareq *);
int 	eou_config(struct ifnet *, struct sockaddr *, struct sockaddr *);


struct eou_header { 
	uint32_t 	eou_network;
	uint16_t	eou_type;
} __packed;
     
#define EOU_T_DATA		0x0000
#define EOU_T_PING		0x8000
#define EOU_T_PONG		0x8001

// Pingpong payload structure
struct eou_pingpong {
	struct eou_header	hdr;
	uint16_t		_pad;
	uint64_t		utime;
	uint8_t 		random[32];
	uint8_t 		mac[8];
} __packed;

#define EOU_HDRLEN 	sizeof(struct eou_pingpong)

struct eou_softc {
	struct arpcom		 sc_ac;
	// { 
	// 		ac_enaddr 	- holds MAC address of the interface
	// }
	struct ifmedia		 sc_media;

	void			*sc_ahcookie;
	void			*sc_lhcookie;
	void			*sc_dhcookie;

	struct sockaddr_storage		sc_src;
	struct sockaddr_storage	 	sc_dst;
	struct socket		*so;
	in_port_t		 sc_dstport;
	u_int			 sc_rdomain;
	u_int32_t		 sc_vnetid;
	u_int8_t		 sc_ttl;

	LIST_ENTRY(eou_softc)	 sc_entry;
};

struct if_clone	eou_cloner =
    IF_CLONE_INITIALIZER("eou", eou_clone_create, eou_clone_destroy);

int
eou_media_change(struct ifnet *ifp)
{
	return (0);
}

void
eou_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	imr->ifm_active = IFM_ETHER | IFM_AUTO;
	imr->ifm_status = IFM_AVALID;
}

void
eouattach(int neou)
{
	if_clone_attach(&eou_cloner);
	printf("eou0: pseudo-device driver has been initialized. ");
}

/*
 * The clone_create function is responsible for allocating the memory 
 * needed for the interfaces data structures, initializing it, 
 * and attaching it to the network stack.
 */
int
eou_clone_create(struct if_clone *ifc, int unit)
{
	// Called on ifconfig eou create
	struct ifnet		*ifp;
	// {
	// 	...
	//	if_xname 	- name of this instance of the interface
	// 	if_flags 	- interface capabilities and state
	//	if_sortc 	- pointer to the interfaces software state
	// 	if_ioctl 	- pointer to the interfaces ioctl function
	//	if_start 	- pointer to the transmit function
	//	if_snd	 	- queue of packets ready for transmission
	//  ...
	// }
	struct eou_softc	*sc;

	// Allocate memory for eou_softc structure
	if ((sc = malloc(sizeof(*sc),
	    M_DEVBUF, M_NOWAIT|M_ZERO)) == NULL)
		return (ENOMEM);

	ifp = &sc->sc_ac.ac_if;
	// Assign name
	snprintf(ifp->if_xname, sizeof ifp->if_xname, "eou%d", unit);
	// Assign interface flags here
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ether_fakeaddr(ifp);

	ifp->if_softc = sc;
	ifp->if_ioctl = eouioctl;
	ifp->if_start = eoustart;
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);
	IFQ_SET_READY(&ifp->if_snd);

	ifp->if_capabilities = IFCAP_VLAN_MTU;

	ifmedia_init(&sc->sc_media, 0, eou_media_change,
	    eou_media_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);

	if_attach(ifp);
	ether_ifattach(ifp);

	if (ifp->if_flags & IFF_DEBUG) {
		printf("[%s] DEBUG: interface has been created. \n",
			ifp->if_xname);
	} 

	return (0);
}


/* 
 * The destroy function is responsible for detaching the interface 
 * from the network stack and freeing any memory associated with it.
 */ 
int
eou_clone_destroy(struct ifnet *ifp)
{
	if (ifp->if_flags & IFF_DEBUG) {
		printf("[%s] DEBUG: destroy device. \n", ifp->if_xname);
	}

	struct eou_softc	*sc = ifp->if_softc;

	ifmedia_delete_instance(&sc->sc_media, IFM_INST_ANY);
	ether_ifdetach(ifp);
	if_detach(ifp);
	free(sc, M_DEVBUF, sizeof(*sc));
	return (0);
}

/*
 * The bridge has magically already done all the work for us,
 * and we only need to discard the packets.
 */
void
eoustart(struct ifnet *ifp)
{
	if (ifp->if_flags & IFF_DEBUG) {
		printf("[%s] DEBUG: start packet transmission. \n", 
			ifp->if_xname);
	}

	struct mbuf		*m;
	int			 s;

	for (;;) {
		s = splnet();
		IFQ_DEQUEUE(&ifp->if_snd, m);
		splx(s);

		if (m == NULL)
			return;
		ifp->if_opackets++;
		m_freem(m);
	}
}

int
eou_config(struct ifnet *ifp, struct sockaddr *src, struct sockaddr *dst)
{
	struct eou_softc	*sc = (struct eou_softc *)ifp->if_softc;
	struct sockaddr_in	*src4, *dst4;
	int			 reset = 0;

	if (src != NULL && dst != NULL) {
		/* XXX inet6 is not supported */
		if (src->sa_family != AF_INET || dst->sa_family != AF_INET)
			return (EAFNOSUPPORT);
	} else {
		/* Reset current configuration */
		src = (struct sockaddr *)&sc->sc_src;
		dst = (struct sockaddr *)&sc->sc_dst;
		reset = 1;
	}

	src4 = satosin(src);
	dst4 = satosin(dst);

	// Check if adresses are valid
	if (src4->sin_len != sizeof(*src4)||dst4->sin_len != sizeof(*dst4))
		return (EINVAL);

	// Assign port if specified 
	if (dst4->sin_port) {
		sc->sc_dstport = dst4->sin_port;

		if(ifp->if_flags & IFF_DEBUG)
			printf("[%s] DEBUG: use user specified port %d.\n",
			ifp->if_xname, ntohs(dst4->sin_port)); 
	} else {
		sc->sc_dstport = (in_port_t) htons(EOU_PORT); 
		// Populate dst->sin_port with default port value
		dst4->sin_port = sc->sc_dstport;
		
		if(ifp->if_flags & IFF_DEBUG)
			printf("[%s] DEBUG: use default port %d.\n", 
			ifp->if_xname, ntohs(sc->sc_dstport));
	}

	// Reset configuration if needed
	if (!reset) {
		bzero(&sc->sc_src, sizeof(sc->sc_src));
		bzero(&sc->sc_dst, sizeof(sc->sc_dst));
		memcpy(&sc->sc_src, src, src->sa_len);
		memcpy(&sc->sc_dst, dst, dst->sa_len);
	}

	return (0);
}


/* ARGSUSED 
* Parameters:
* ifp - interface descriptor
* cmd - a request code number
*/
int
eouioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	// Access data about this pseudo-device
	struct eou_softc	*sc = (struct eou_softc *)ifp->if_softc;
	struct eou_header	 h;
	struct ifaddr		*ifa = (struct ifaddr *)data;
	struct ifreq		*ifr = (struct ifreq *)data;
	struct if_laddrreq	*lifr = (struct if_laddrreq *)data;
	struct socket		*so; /* Socket */
	struct mbuf			*m; 
	struct sockaddr		*sa;
	struct proc			*p = curproc;
	int			 error = 0, link_state, s;

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (ifa->ifa_addr->sa_family == AF_INET)
			arp_ifinit(&sc->sc_ac, ifa);
			/* FALLTHROUGH */

	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			// If IFF_UP is true, 
			ifp->if_flags |= IFF_RUNNING;
			link_state = LINK_STATE_UP;
		} else {
			ifp->if_flags &= ~IFF_RUNNING;
			link_state = LINK_STATE_DOWN;
		}

		if (ifp->if_link_state != link_state) {
			ifp->if_link_state = link_state;
			if_link_state_change(ifp);
		}
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;

	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;

	// Set new source and destination address
	case SIOCSLIFPHYADDR:
		// Get software lock
		s = splnet();
		// Set source and destination information
		error = eou_config(ifp,
		    (struct sockaddr *)&lifr->addr,
		    (struct sockaddr *)&lifr->dstaddr);

		// try to connect to socket if configured with no errors
		if (!error) {
			// socreate -> sobind -> soconnect pipeline
			//1. Socreate - create new socket
			if (sc->so == NULL) {
				printf("Creating socket. sa_family: %d. \n",
					sc->sc_dst.ss_family);
				error = socreate(sc->sc_dst.ss_family,
				    &so, SOCK_DGRAM, 17);

				if (error) {
					printf("Failed to create a socket. Error: %d \n", error);
					break;
				} else {
					printf("Socket created without errors.\n");
				}

				//2. Sobind - bind socket locally
				// Allocate source address to mbuf
				MGET(m, M_WAIT, MT_SONAME);
				m->m_len = sc->sc_src.ss_len;
				sa = mtod(m, struct sockaddr *);
				memcpy(sa, &sc->sc_src,
				    sc->sc_src.ss_len);

				error = sobind(so, m, p);
				m_freem(m);
				if (error) {
					printf("Failed to bind socket. Error: %d \n", error);
					soclose(so);
					splx(s);
					return (error);
				} else {
					printf("Socket binding successful. \n");
				}

				// 3. Soconnect - connect to destination
				// Allocate source address to mbuf
				MGET(m, M_WAIT, MT_SONAME);
				m->m_len = sc->sc_dst.ss_len;
				sa = mtod(m, struct sockaddr *);
				// - Fill the second m_buf with the dst
				memcpy(sa, &sc->sc_dst,
				    sc->sc_dst.ss_len);
				// - Connect to the socket and the dst.
				error = soconnect(so, m);
				if (error) {
					printf("Failed to connect socket to destination. Error: %d \n", error);
					soclose(so);
					splx(s);
					return (error);
				} else {
					printf("Socket successfuly connected to destination. \n");
				}
				m_freem(m);

				// Configure device media state and link state

				
				// Get packet with header
				MGETHDR(m, M_DONTWAIT, MT_DATA);
				if (m == NULL) {
					printf("Cannot get a packet with header.\n");
					return (error);
				}

				m->m_len = m->m_pkthdr.len = 0;

				h.eou_type = htons(EOU_T_PING);
				m_copyback(m, 0, EOU_HDRLEN, &h, M_NOWAIT);

				// getnanotime(&tv);
				// h->time_sec = htonl(tv.tv_sec);			/* XXX 2038 */
				// h->time_nanosec = htonl(tv.tv_nsec);
				if (sc->so == NULL) {
					m_freem(m);
					return (EINVAL);
				}

				// int
				// sosend(struct socket *so, struct mbuf *addr, struct uio *uio, struct mbuf *top, struct mbuf *control, int flags);
				error = sosend(sc->so, m, NULL, m, NULL, 0);

				if (error) {
					printf("Failed to send data to socket.\n");
				} else {
					printf("Ping was sent to socket.\n");
				}
			} else {
				printf("Socket already exists.\n");
			}
		}

		splx(s);
		break;

	// Remove source and destination address
	case SIOCDIFPHYADDR:
		s = splnet();
		// Fill source and destination values with zeros
		bzero(&sc->sc_src, sizeof(sc->sc_src));
		bzero(&sc->sc_dst, sizeof(sc->sc_dst));
		sc->sc_dstport = htons(EOU_PORT);
		splx(s);
		break;

	// Get source and destination address 
	case SIOCGLIFPHYADDR:
		if (sc->sc_dst.ss_family == AF_UNSPEC) {
			error = EADDRNOTAVAIL;
			break;
		}
		// Fill source and destination with zeros first
		bzero(&lifr->addr, sizeof(lifr->addr));
		bzero(&lifr->dstaddr, sizeof(lifr->dstaddr));
		// Populate them with actual source and destination values
		memcpy(&lifr->addr, &sc->sc_src, sc->sc_src.ss_len);
		memcpy(&lifr->dstaddr, &sc->sc_dst, sc->sc_dst.ss_len);
		break;

	// Set VNETID
	case SIOCSVNETID:
		if (ifp->if_flags & IFF_DEBUG)
			printf("[%s] DEBUG: try to set vnetid to %d.\n", 
				ifp->if_xname, ifr->ifr_vnetid);
		
		// TODO Check if user is superuser
		if (ifr->ifr_vnetid < 0 || ifr->ifr_vnetid > 0x00ffffff) {
			error = EINVAL;
			break;
		}

		// aquire software lock
		s = splnet();
		// Get vnetid from interface and assign it to sc
		sc->sc_vnetid = (u_int32_t)ifr->ifr_vnetid;
		// Release lock
		splx(s);
		
		if (ifp->if_flags & IFF_DEBUG)
			printf("[%s] DEBUG: vnetid has been set to %d",
				ifp->if_xname, (int) sc->sc_vnetid);
		
		break;

	case SIOCGVNETID:
		// Return VNETID back to the interface
		ifr->ifr_vnetid = (int)sc->sc_vnetid;
		if (ifp->if_flags & IFF_DEBUG) {
			printf("[%s] DEBUG: vnetid requested: %d. \n",
				ifp->if_xname, ifr->ifr_vnetid);
		}
		break;

	default:
		error = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
	}
	return (error);
}
