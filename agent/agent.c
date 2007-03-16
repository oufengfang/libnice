
#include <string.h>

#include <sys/select.h>

#include <glib.h>

#include "stun.h"
#include "udp.h"
#include "agent.h"
#include "agent-signals-marshal.h"
#include "stream.h"


/*** candidate_pair ***/


typedef struct _CandidatePair CandidatePair;

struct _CandidatePair
{
  NiceCandidate *local;
  NiceCandidate *remote;
};


/* ICE-13 §5.7 (p24) */
typedef enum
{
  CHECK_STATE_WAITING,
  CHECK_STATE_IN_PROGRESS,
  CHECK_STATE_SUCCEEDED,
  CHECK_STATE_FAILED,
  CHECK_STATE_FROZEN,
} CheckState;


typedef enum
{
  CHECK_LIST_STATE_RUNNING,
  CHECK_LIST_STATE_COMPLETED,
} CheckListState;


#if 0
/* ICE-13 §5.7 */
guint64
candidate_pair_priority (
      guint64 offerer_prio,
      guint64 answerer_prio)
{
  return (
      0x100000000LL * MIN (offerer_prio, answerer_prio) +
      2 * MAX (offerer_prio, answerer_prio) +
      (offerer_prio > answerer_prio ? 1 : 0));
}
#endif


/*** agent ***/


G_DEFINE_TYPE (NiceAgent, nice_agent, G_TYPE_OBJECT);


enum
{
  PROP_SOCKET_FACTORY = 1,
  PROP_STUN_SERVER
};


enum
{
  SIGNAL_COMPONENT_STATE_CHANGED,
  N_SIGNALS,
};


static guint signals[N_SIGNALS];


static Stream *
find_stream (NiceAgent *agent, guint stream_id)
{
  GSList *i;

  for (i = agent->streams; i; i = i->next)
    {
      Stream *s = i->data;

      if (s->id == stream_id)
        return s;
    }

  return NULL;
}


static gboolean
find_component (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  Stream **stream,
  Component **component)
{
  Stream *s;

  if (component_id != 1)
    return FALSE;

  s = find_stream (agent, stream_id);

  if (s == NULL)
    return FALSE;

  if (stream)
    *stream = s;

  if (component)
    *component = s->component;

  return TRUE;
}


static void
nice_agent_dispose (GObject *object);

static void
nice_agent_get_property (
  GObject *object,
  guint property_id,
  GValue *value,
  GParamSpec *pspec);

static void
nice_agent_set_property (
  GObject *object,
  guint property_id,
  const GValue *value,
  GParamSpec *pspec);


static void
nice_agent_class_init (NiceAgentClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = nice_agent_get_property;
  gobject_class->set_property = nice_agent_set_property;
  gobject_class->dispose = nice_agent_dispose;

  /* install properties */

  g_object_class_install_property (gobject_class, PROP_SOCKET_FACTORY,
      g_param_spec_pointer (
         "socket-factory",
         "UDP socket factory",
         "The socket factory used to create new UDP sockets",
         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_STUN_SERVER,
      g_param_spec_string (
        "stun-server",
        "STUN server",
        "The STUN server used to obtain server-reflexive candidates",
        NULL,
        G_PARAM_READWRITE));

  /* install signals */

  signals[SIGNAL_COMPONENT_STATE_CHANGED] =
      g_signal_new (
          "component-state-changed",
          G_OBJECT_CLASS_TYPE (klass),
          G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
          0,
          NULL,
          NULL,
          agent_marshal_VOID__UINT_UINT_UINT,
          G_TYPE_NONE,
          3,
          G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
          G_TYPE_INVALID);
}


static void
nice_agent_init (NiceAgent *agent)
{
  agent->next_candidate_id = 1;
  agent->next_stream_id = 1;
  agent->rng = nice_rng_new ();
}


/**
 * nice_agent_new:
 * @factory: a NiceUDPSocketFactory used for allocating sockets
 *
 * Create a new NiceAgent.
 *
 * Returns: the new agent
 **/
NiceAgent *
nice_agent_new (NiceUDPSocketFactory *factory)
{
  return g_object_new (NICE_TYPE_AGENT,
      "socket-factory", factory,
      NULL);
}


static void
nice_agent_get_property (
  GObject *object,
  guint property_id,
  GValue *value,
  GParamSpec *pspec)
{
  NiceAgent *agent = NICE_AGENT (object);

  switch (property_id)
    {
    case PROP_SOCKET_FACTORY:
      g_value_set_pointer (value, agent->socket_factory);
      break;

    case PROP_STUN_SERVER:
      g_value_set_string (value, agent->stun_server);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
nice_agent_set_property (
  GObject *object,
  guint property_id,
  const GValue *value,
  GParamSpec *pspec)
{
  NiceAgent *agent = NICE_AGENT (object);

  switch (property_id)
    {
    case PROP_SOCKET_FACTORY:
      agent->socket_factory = g_value_get_pointer (value);
      break;

    case PROP_STUN_SERVER:
      agent->stun_server = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
nice_agent_add_local_host_candidate (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  NiceAddress *address)
{
  NiceCandidate *candidate;
  Component *component;

  if (!find_component (agent, stream_id, component_id, NULL, &component))
    return;

  candidate = nice_candidate_new (NICE_CANDIDATE_TYPE_HOST);
  candidate->id = agent->next_candidate_id++;
  candidate->stream_id = stream_id;
  candidate->component_id = component_id;
  candidate->addr = *address;
  candidate->base_addr = *address;
  component->local_candidates = g_slist_append (component->local_candidates,
      candidate);

  /* generate username/password */
  nice_rng_generate_bytes_print (agent->rng, 8, candidate->username);
  nice_rng_generate_bytes_print (agent->rng, 8, candidate->password);

  /* allocate socket */
  /* XXX: handle error */
  if (!nice_udp_socket_factory_make (agent->socket_factory,
        &(candidate->sock), address))
    g_assert_not_reached ();

  candidate->addr = candidate->sock.addr;
  candidate->base_addr = candidate->sock.addr;
}


/**
 * nice_agent_add_stream:
 *  @agent: a NiceAgent
 *  @handle_recv: a function called when the stream recieves data
 *  @handle_recv_data: data passed as last parameter to @handle_recv
 *
 * Add a data stream to @agent.
 *
 * Returns: the ID of the new stream
 **/
guint
nice_agent_add_stream (
  NiceAgent *agent,
  guint n_components)
{
  Stream *stream;
  GSList *i;

  g_assert (n_components == 1);
  stream = stream_new ();
  stream->id = agent->next_stream_id++;
  agent->streams = g_slist_append (agent->streams, stream);

  /* generate a local host candidate for each local address */

  for (i = agent->local_addresses; i; i = i->next)
    {
      NiceAddress *addr = i->data;

      nice_agent_add_local_host_candidate (agent, stream->id,
          stream->component->id, addr);

      /* XXX: need to check for redundant candidates? */
      /* later: send STUN requests to obtain server-reflexive candidates */
    }

  return stream->id;
}


/**
 * nice_agent_remove_stream:
 *  @agent: a NiceAgent
 *  @stream_id: the ID of the stream to remove
 **/
void
nice_agent_remove_stream (
  NiceAgent *agent,
  guint stream_id)
{
  /* note that streams/candidates can be in use by other threads */

  Stream *stream;

  stream = find_stream (agent, stream_id);

  if (!stream)
    return;

  /* remove stream */

  stream_free (stream);
  agent->streams = g_slist_remove (agent->streams, stream);
}


/**
 * nice_agent_add_local_address:
 *  @agent: A NiceAgent
 *  @addr: the address of a local IP interface
 *
 * Inform the agent of the presence of an address that a local network
 * interface is bound to.
 **/
void
nice_agent_add_local_address (NiceAgent *agent, NiceAddress *addr)
{
  NiceAddress *dup;

  dup = nice_address_dup (addr);
  dup->port = 0;
  agent->local_addresses = g_slist_append (agent->local_addresses, dup);

  /* XXX: Should we generate local candidates for existing streams at this
   * point, or require that local addresses are set before media streams are
   * added?
   */
}

/**
 * nice_agent_add_remote_candidate
 *  @agent: a NiceAgent
 *  @stream_id: the ID of the stream the candidate is for
 *  @component_id: the ID of the component the candidate is for
 *  @type: the type of the new candidate
 *  @addr: the new candidate's IP address
 *  @port: the new candidate's port
 *  @username: the new candidate's username
 *  @password: the new candidate's password
 *
 * Add a candidate our peer has informed us about to the agent's list.
 **/
void
nice_agent_add_remote_candidate (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  NiceCandidateType type,
  NiceAddress *addr,
  const gchar *username,
  const gchar *password)
{
  NiceCandidate *candidate;
  Component *component;

  if (!find_component (agent, stream_id, component_id, NULL, &component))
    return;

  candidate = nice_candidate_new (type);
  candidate->stream_id = stream_id;
  candidate->component_id = component_id;
  /* XXX: do remote candidates need IDs? */
  candidate->id = 0;
  candidate->addr = *addr;
  strncpy (candidate->username, username, sizeof (candidate->username));
  strncpy (candidate->password, password, sizeof (candidate->password));

  component->remote_candidates = g_slist_append (component->remote_candidates,
      candidate);

  /* later: for each component, generate a new check with the new candidate */
}


#if 0
static NiceCandidate *
_local_candidate_lookup (NiceAgent *agent, guint candidate_id)
{
  GSList *i;

  for (i = agent->local_candidates; i; i = i->next)
    {
      NiceCandidate *c = i->data;

      if (c->id == candidate_id)
        return c;
    }

  return NULL;
}
#endif


static NiceCandidate *
find_candidate_by_fd (Component *component, guint fd)
{
  GSList *i;

  for (i = component->local_candidates; i; i = i->next)
    {
      NiceCandidate *c = i->data;

      if (c->sock.fileno == fd)
        return c;
    }

  return NULL;
}


static void
_handle_stun_binding_request (
  NiceAgent *agent,
  Stream *stream,
  Component *component,
  NiceCandidate *local,
  NiceAddress from,
  StunMessage *msg)
{
  GSList *i;
  StunAttribute *attr;
  gchar *username = NULL;
  NiceCandidate *remote = NULL;

  /* msg should have either:
   *
   *   Jingle P2P:
   *     username = local candidate username + remote candidate username
   *   ICE:
   *     username = local candidate username + ":" + remote candidate username
   *     password = local candidate pwd
   *     priority = priority to use if a new candidate is generated
   *
   * Note that:
   *
   *  - "local"/"remote" are from the perspective of the receiving side
   *  - the remote candidate username is not necessarily unique; Jingle seems
   *    to always generate a unique username/password for each candidate, but
   *    ICE makes no guarantees
   *
   * There are three cases we need to deal with:
   *
   *  - valid username with a known address
   *    --> send response
   *  - valid username with an unknown address
   *    --> send response
   *    --> later: create new remote candidate
   *  - invalid username
   *    --> send error
   */

  attr = stun_message_find_attribute (msg, STUN_ATTRIBUTE_USERNAME);

  if (attr == NULL)
    /* no username attribute found */
    goto ERROR;

  username = attr->username;

  /* validate username */
  /* XXX: Should first try and find a remote candidate with a matching
   * transport address, and fall back to matching on username only after that.
   * That way, we know to always generate a new remote candidate if the
   * transport address didn't match.
   */

  for (i = component->remote_candidates; i; i = i->next)
    {
      guint len;

      remote = i->data;

#if 0
      g_debug ("uname check: %s :: %s -- %s", username, local->username,
          remote->username);
#endif

      if (!g_str_has_prefix (username, local->username))
        continue;

      len = strlen (local->username);

      if (0 != strcmp (username + len, remote->username))
        continue;

#if 0
      /* usernames match; check address */

      if (rtmp->addr.addr_ipv4 == ntohs (from.sin_addr.s_addr) &&
          rtmp->port == ntohl (from.sin_port))
        {
          /* this is a candidate we know about, just send a reply */
          /* is candidate pair active now? */
          remote = rtmp;
        }
#endif

      /* send response */
      goto RESPOND;
    }

  /* username is not valid */
  goto ERROR;

RESPOND:

#ifdef DEBUG
    {
      gchar ip[NICE_ADDRESS_STRING_LEN];

      nice_address_to_string (&remote->addr, ip);
      g_debug ("s%d:%d: got valid connectivity check for candidate %d (%s:%d)",
          stream->id, component->id, remote->id, ip, remote->addr.port);
    }
#endif

  /* update candidate/peer affinity */
  /* Note that @from might be different to @remote->addr; for ICE, this
   * (always?) creates a new peer-reflexive remote candidate (§7.2).
   */
  /* XXX: test case where @from != @remote->addr. */

  component->active_candidate = local;
  component->peer_addr = from;

  /* send STUN response */

    {
      StunMessage *response;
      guint len;
      gchar *packed;

      response = stun_message_new (STUN_MESSAGE_BINDING_RESPONSE,
          msg->transaction_id, 2);
      response->attributes[0] = stun_attribute_mapped_address_new (
          from.addr_ipv4, from.port);
      response->attributes[1] = stun_attribute_username_new (username);
      len = stun_message_pack (response, &packed);
      nice_udp_socket_send (&local->sock, &from, len, packed);

      g_free (packed);
      stun_message_free (response);
    }

  /* send reciprocal ("triggered") connectivity check */
  /* XXX: possibly we shouldn't do this if we're being an ICE Lite agent */

    {
      StunMessage *extra;
      gchar *username;
      guint len;
      gchar *packed;

      extra = stun_message_new (STUN_MESSAGE_BINDING_REQUEST,
          NULL, 1);

      username = g_strconcat (remote->username, local->username, NULL);
      extra->attributes[0] = stun_attribute_username_new (username);
      g_free (username);

      nice_rng_generate_bytes (agent->rng, 16, extra->transaction_id);

      len = stun_message_pack (extra, &packed);
      nice_udp_socket_send (&local->sock, &from, len, packed);
      g_free (packed);

      stun_message_free (extra);
    }

  /* emit component-state-changed(connected) */
  /* XXX: probably better do this when we get the binding response */

    {
      if (component->state != NICE_COMPONENT_STATE_CONNECTED)
        {
          component->state = NICE_COMPONENT_STATE_CONNECTED;
          g_signal_emit (agent, signals[SIGNAL_COMPONENT_STATE_CHANGED], 0,
              stream->id, component->id, component->state);
        }
    }

  return;

ERROR:

#ifdef DEBUG
    {
      gchar ip[NICE_ADDRESS_STRING_LEN];

      nice_address_to_string (&remote->addr, ip);
      g_debug (
          "s%d:%d: got invalid connectivity check for candidate %d (%s:%d)",
          stream->id, component->id, remote->id, ip, remote->addr.port);
    }
#endif

  /* XXX: add ERROR-CODE parameter */

    {
      StunMessage *response;
      guint len;
      gchar *packed;

      response = stun_message_new (STUN_MESSAGE_BINDING_ERROR_RESPONSE,
          msg->transaction_id, 0);
      len = stun_message_pack (response, &packed);
      nice_udp_socket_send (&local->sock, &from, len, packed);

      g_free (packed);
      stun_message_free (response);
    }

  /* XXX: we could be clever and keep around STUN packets that we couldn't
   * validate, then re-examine them when we get new remote candidates -- would
   * this fix some timing problems (i.e. TCP being slower than UDP)
   */
  /* XXX: if the peer is the controlling agent, it may include a USE-CANDIDATE
   * attribute in the binding request
   */
}


static void
_handle_stun (
  NiceAgent *agent,
  Stream *stream,
  Component *component,
  NiceCandidate *local,
  NiceAddress from,
  StunMessage *msg)
{
  switch (msg->type)
    {
    case STUN_MESSAGE_BINDING_REQUEST:
      _handle_stun_binding_request (agent, stream, component, local, from,
          msg);
      break;
    case STUN_MESSAGE_BINDING_RESPONSE:
      /* XXX: check it matches a request we sent */
      break;
    default:
      /* a message type we don't know how to handle */
      /* XXX: send error response */
      break;
    }
}


static guint
_nice_agent_recv (
  NiceAgent *agent,
  Stream *stream,
  Component *component,
  NiceCandidate *candidate,
  guint buf_len,
  gchar *buf)
{
  NiceAddress from;
  guint len;

  len = nice_udp_socket_recv (&(candidate->sock), &from,
      buf_len, buf);

  if (len == 0)
    return 0;

  if (len > buf_len)
    {
      /* buffer is not big enough to accept this packet */
      /* XXX: test this case */
      return 0;
    }

  /* XXX: verify sender; maybe:
   * 
   * if (candidate->other != NULL)
   *   {
   *     if (from != candidate->other.addr)
   *       // ignore packet from unexpected sender
   *       return;
   *   }
   * else
   *   {
   *     // go through remote candidates, looking for one matching packet from
   *     // address; if found, assign it to candidate->other and call handler,
   *     // otherwise ignore it
   *   }
   *
   * Perhaps remote socket affinity is superfluous and all we need is the
   * second part.
   * Perhaps we should also check whether this candidate is supposed to be
   * active.
   */

  /* The top two bits of an RTP message are the version number; the current
   * version number is 2. The top two bits of a STUN message are always 0.
   */

  if ((buf[0] & 0xc0) == 0x80)
    {
      /* looks like RTP */
      return len;
    }
  else if ((buf[0] & 0xc0) == 0)
    {
      /* looks like a STUN message (connectivity check) */
      /* connectivity checks are described in ICE-13 §7. */
      StunMessage *msg;

      msg = stun_message_unpack (len, buf);

      if (msg != NULL)
        {
          _handle_stun (agent, stream, component, candidate, from, msg);
          stun_message_free (msg);
        }
    }

  /* anything else is ignored */
  return 0;
}


/**
 * nice_agent_recv:
 *  @agent: a NiceAgent
 *  @stream_id: the ID of the stream to recieve data from
 *  @component_id: the ID of the component to receive data from
 *  @buf_len: the size of @buf
 *  @buf: the buffer to read data into
 *
 * Recieve data on a particular component.
 *
 * Returns: the amount of data read into @buf
 **/
guint
nice_agent_recv (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  guint buf_len,
  gchar *buf)
{
  guint len = 0;
  fd_set fds;
  guint max_fd = 0;
  gint num_readable;
  GSList *i;
  Stream *stream;
  Component *component;

  if (!find_component (agent, stream_id, component_id, &stream, &component))
    return 0;

  FD_ZERO (&fds);

  for (i = component->local_candidates; i; i = i->next)
    {
      NiceCandidate *candidate = i->data;

      FD_SET (candidate->sock.fileno, &fds);
      max_fd = MAX (candidate->sock.fileno, max_fd);
    }

  /* Loop on candidate sockets until we find one that has non-STUN data
   * waiting on it.
   */

  for (;;)
    {
      num_readable = select (max_fd + 1, &fds, NULL, NULL, NULL);
      g_assert (num_readable >= 0);

      if (num_readable > 0)
        {
          guint j;

          for (j = 0; j <= max_fd; j++)
            if (FD_ISSET (j, &fds))
              {
                NiceCandidate *candidate;

                candidate = find_candidate_by_fd (component, j);
                g_assert (candidate);
                len = _nice_agent_recv (agent, stream, component, candidate,
                    buf_len, buf);

                if (len > 0)
                  return len;
              }
        }
    }

  g_assert_not_reached ();
}


guint
nice_agent_recv_sock (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  guint sock,
  guint buf_len,
  gchar *buf)
{
  NiceCandidate *candidate;
  Stream *stream;
  Component *component;

  if (!find_component (agent, stream_id, component_id, &stream, &component))
    return 0;

  candidate = find_candidate_by_fd (component, sock);
  g_assert (candidate);

  return _nice_agent_recv (agent, stream, stream->component,
      candidate, buf_len, buf);
}


/**
 * nice_agent_poll_read:
 *  @agent: A NiceAgent
 *  @other_fds: A GSList of other file descriptors to poll
 *
 * Polls the agent's sockets until at least one of them is readable, and
 * additionally if @other_fds is not NULL, polls those for readability too.
 * @other_fds should contain the file descriptors directly, i.e. using
 * GUINT_TO_POINTER.
 *
 * Returns: A list of file descriptors from @other_fds that are readable
 **/
GSList *
nice_agent_poll_read (
  NiceAgent *agent,
  GSList *other_fds,
  NiceAgentRecvFunc func,
  gpointer data)
{
  fd_set fds;
  guint max_fd = 0;
  gint num_readable;
  GSList *ret = NULL;
  GSList *i;
  guint j;

  FD_ZERO (&fds);

  for (i = agent->streams; i; i = i->next)
    {
      GSList *j;
      Stream *stream = i->data;
      Component *component = stream->component;

      for (j = component->local_candidates; j; j = j->next)
        {
          NiceCandidate *candidate = j->data;

          FD_SET (candidate->sock.fileno, &fds);
          max_fd = MAX (candidate->sock.fileno, max_fd);
        }
    }

  for (i = other_fds; i; i = i->next)
    {
      guint fileno;

      fileno = GPOINTER_TO_UINT (i->data);
      FD_SET (fileno, &fds);
      max_fd = MAX (fileno, max_fd);
    }

  num_readable = select (max_fd + 1, &fds, NULL, NULL, NULL);

  if (num_readable < 1)
    /* none readable, or error */
    return NULL;

  for (j = 0; j <= max_fd; j++)
    if (FD_ISSET (j, &fds))
      {
        if (g_slist_find (other_fds, GUINT_TO_POINTER (j)))
          ret = g_slist_append (ret, GUINT_TO_POINTER (j));
        else
          {
            NiceCandidate *candidate = NULL;
            Stream *stream;
            gchar buf[1024];
            guint len;

            for (i = agent->streams; i; i = i->next)
              {
                Stream *s = i->data;
                Component *c = s->component;

                candidate = find_candidate_by_fd (c, j);

                if (candidate != NULL)
                  break;
              }

            if (candidate == NULL)
              break;

            stream = find_stream (agent, candidate->stream_id);

            if (stream == NULL)
              break;

            len = _nice_agent_recv (agent, stream, stream->component,
                candidate, 1024, buf);

            if (len && func != NULL)
              func (agent, stream->id, candidate->component_id, len, buf,
                  data);
          }
      }

  return ret;
}


void
nice_agent_send (
  NiceAgent *agent,
  guint stream_id,
  guint component_id,
  guint len,
  const gchar *buf)
{
  Stream *stream;
  Component *component;

  stream = find_stream (agent, stream_id);
  component = stream->component;

  if (component->active_candidate != NULL)
    {
      NiceUDPSocket *sock;
      NiceAddress *addr;

#if 0
      g_debug ("s%d:%d: sending %d bytes to %08x:%d", stream_id, component_id,
          len, component->peer_addr->addr_ipv4, component->peer_addr->port);
#endif

      sock = &component->active_candidate->sock;
      addr = &component->peer_addr;
      nice_udp_socket_send (sock, addr, len, buf);
    }
}


/**
 * Set the STUN server from which to obtain server-reflexive candidates.
 */
/*
void
nice_agent_set_stun_server (
  NiceAgent *agent,
  NiceAddress *addr)
{
}
*/


/**
 * nice_agent_get_local_candidates:
 *  @agent: A NiceAgent
 *
 * The caller owns the returned GSList but not the candidates contained within
 * it.
 *
 * Returns: a GSList of local candidates belonging to @agent
 **/
GSList *
nice_agent_get_local_candidates (
  NiceAgent *agent,
  guint stream_id,
  guint component_id)
{
  Component *component;

  if (!find_component (agent, stream_id, component_id, NULL, &component))
    return NULL;

  return g_slist_copy (component->local_candidates);
}


/**
 * nice_agent_get_remote_candidates:
 *  @agent: A NiceAgent
 *
 * The caller owns the returned GSList but not the candidates contained within
 * it.
 *
 * Returns: a GSList of remote candidates belonging to @agent
 **/
GSList *
nice_agent_get_remote_candidates (
  NiceAgent *agent,
  guint stream_id,
  guint component_id)
{
  Component *component;

  if (!find_component (agent, stream_id, component_id, NULL, &component))
    return NULL;

  return g_slist_copy (component->remote_candidates);
}


static void
nice_agent_dispose (GObject *object)
{
  GSList *i;
  NiceAgent *agent = NICE_AGENT (object);

  for (i = agent->local_addresses; i; i = i->next)
    {
      NiceAddress *a = i->data;

      nice_address_free (a);
    }

  g_slist_free (agent->local_addresses);
  agent->local_addresses = NULL;

  for (i = agent->streams; i; i = i->next)
    {
      Stream *s = i->data;

      stream_free (s);
    }

  g_slist_free (agent->streams);
  agent->streams = NULL;

  g_free (agent->stun_server);
  agent->stun_server = NULL;

  nice_rng_free (agent->rng);
  agent->rng = NULL;

  if (G_OBJECT_CLASS (nice_agent_parent_class)->dispose)
    G_OBJECT_CLASS (nice_agent_parent_class)->dispose (object);
}


typedef struct _IOCtx IOCtx;

struct _IOCtx
{
  NiceAgent *agent;
  Stream *stream;
  Component *component;
  NiceCandidate *candidate;
};


static IOCtx *
io_ctx_new (
  NiceAgent *agent,
  Stream *stream,
  Component *component,
  NiceCandidate *candidate)
{
  IOCtx *ctx;

  ctx = g_slice_new0 (IOCtx);
  ctx->agent = agent;
  ctx->stream = stream;
  ctx->component = component;
  ctx->candidate = candidate;
  return ctx;
}


static void
io_ctx_free (IOCtx *ctx)
{
  g_slice_free (IOCtx, ctx);
}


static gboolean
nice_agent_g_source_cb (
  GIOChannel *source,
  G_GNUC_UNUSED
  GIOCondition condition,
  gpointer data)
{
  /* return value is whether to keep the source */

  IOCtx *ctx = data;
  NiceAgent *agent = ctx->agent;
  Stream *stream = ctx->stream;
  Component *component = ctx->component;
  NiceCandidate *candidate = ctx->candidate;
  gchar buf[1024];
  guint len;

  len = _nice_agent_recv (agent, stream, component, candidate, 1024,
      buf);

  if (len > 0)
    agent->read_func (agent, candidate->stream_id, candidate->component_id,
        len, buf, agent->read_func_data);

  return TRUE;
}


gboolean
nice_agent_main_context_attach (
  NiceAgent *agent,
  GMainContext *ctx,
  NiceAgentRecvFunc func,
  gpointer data)
{
  GSList *i;

  if (agent->main_context_set)
    return FALSE;

  /* attach candidates */

  for (i = agent->streams; i; i = i->next)
    {
      GSList *j;
      Stream *stream = i->data;
      Component *component = stream->component;

      for (j = component->local_candidates; j; j = j->next)
        {
          NiceCandidate *candidate = j->data;
          GIOChannel *io;
          GSource *source;
          IOCtx *ctx;

          io = g_io_channel_unix_new (candidate->sock.fileno);
          source = g_io_create_watch (io, G_IO_IN);
          ctx = io_ctx_new (agent, stream, component, candidate);
          g_source_set_callback (source, (GSourceFunc) nice_agent_g_source_cb,
              ctx, (GDestroyNotify) io_ctx_free);
          g_source_attach (source, NULL);
          candidate->source = source;
        }
    }

  agent->main_context = ctx;
  agent->main_context_set = TRUE;
  agent->read_func = func;
  agent->read_func_data = data;
  return TRUE;
}

