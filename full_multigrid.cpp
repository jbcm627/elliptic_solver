#include "full_multigrid.h"
#include "../../utils/math.h"

namespace cosmo
{

/**
 * @brief Method to initialize internal variables, allocate memory
 * @param[in]  input arrays, has its initial value at finest grid, so need no memory
 * @param[in]  number of variables, equals to number of equations
 * @param[in]  array stores term number for each equation
 * @param[in]  set how many layers we want
 * @param[in]  set number of interations for each relaxation
 * @param[in]  set relaxation jump out precision
 */
FASMultigrid::FASMultigrid(fas_heirarchy_t u_in, idx_t u_n_in, idx_t molecule_n_in [],
              idx_t max_depth_in, idx_t max_relax_iters_in,  real_t relaxation_tolerance_in)
{
  relax_scheme = relax_t::inexact_newton;

  max_relax_iters = max_relax_iters_in;
  max_depth = max_depth_in;
  min_depth = 1;
  max_depth_idx = _dIdx(max_depth);
  min_depth_idx = _dIdx(min_depth);
  total_depths = max_depth - min_depth + 1;
  relaxation_tolerance = relaxation_tolerance_in;
  u_n = u_n_in;
  
  molecule_n = molecule_n_in;
  
  u_h = new fas_heirarchy_t[u_n_in];
  coarse_src_h = new fas_heirarchy_t[u_n_in];
  damping_v_h = new fas_heirarchy_t[u_n_in];
  jac_rhs_h = new fas_heirarchy_t[u_n_in];
  tmp_h = new fas_heirarchy_t[u_n_in];

  eqns = new molecule *[u_n_in];

  rho_h = new fas_heirarchy_set_t[u_n];
  
  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    u_h[eqn_id] = new fas_grid_t[total_depths];
    coarse_src_h[eqn_id] = new fas_grid_t[total_depths];
    damping_v_h[eqn_id] = new fas_grid_t[total_depths];
    jac_rhs_h[eqn_id] = new fas_grid_t[total_depths];
    tmp_h[eqn_id] = new fas_grid_t[total_depths];
    
    rho_h[eqn_id] = new fas_heirarchy_t[molecule_n[eqn_id]];
    
    nx_h = new idx_t[total_depths];
    ny_h = new idx_t[total_depths];
    nz_h = new idx_t[total_depths];

    eqns[eqn_id] = new molecule[molecule_n[eqn_id]]; 
    
    for(idx_t depth = max_depth; depth >= min_depth; --depth)
    {
      idx_t depth_idx = _dIdx(depth);
      if(depth_idx == _dIdx(max_depth))
      {
        u_h[eqn_id][depth_idx]._array = u_in[eqn_id]._array;

        nx_h[depth_idx] = NX;
        ny_h[depth_idx] = NY;
        nz_h[depth_idx] = NZ;
        
        u_h[eqn_id][depth_idx].nx = nx_h[depth_idx];
        u_h[eqn_id][depth_idx].ny = ny_h[depth_idx];
        u_h[eqn_id][depth_idx].nz = nz_h[depth_idx];
        u_h[eqn_id][depth_idx].pts = nx_h[depth_idx] * ny_h[depth_idx] * nz_h[depth_idx];
      }
      else
      {
        nx_h[depth_idx] = nx_h[depth_idx+1] / 2 + (nx_h[depth_idx+1] % 2);
        ny_h[depth_idx] = ny_h[depth_idx+1] / 2 + (ny_h[depth_idx+1] % 2);
        nz_h[depth_idx] = nz_h[depth_idx+1] / 2 + (nz_h[depth_idx+1] % 2);

        u_h[eqn_id][depth_idx].init(nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);
      }
      
      coarse_src_h[eqn_id][depth_idx].init(nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);

      damping_v_h[eqn_id][depth_idx].init(nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);
      
      jac_rhs_h[eqn_id][depth_idx].init(nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);

      tmp_h[eqn_id][depth_idx].init(nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);
    }

    for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
      rho_h[eqn_id][mol_id] = new fas_grid_t[total_depths];
  }
  
  // initializing x, y and z derivative
  der_type[der1][0] = 1;
  der_type[der2][0] = 2;
  der_type[der3][0] = 3;

  // initializing 9 kinds of double derivative
  der_type[der11][0] = 1;
  der_type[der11][1] = 1;

  der_type[der22][0] = 2;
  der_type[der22][1] = 2;

  der_type[der33][0] = 3;
  der_type[der33][1] = 3;

  der_type[der12][0] = 1;
  der_type[der12][1] = 2;

  der_type[der13][0] = 1;
  der_type[der13][1] = 3;

  der_type[der23][0] = 2;
  der_type[der23][1] = 3;

  // type == 11 means laplacian!

  // initilizing coeficient of double derivative for different sencil orders
  double_der_coef[2] = 2.0;
  double_der_coef[4] = 2.5;
  double_der_coef[6] = 49.0 / 18.0;
  double_der_coef[8] = 205.0 / 72.0;
}


void FASMultigrid::add_atom_to_eqn(atom atom_in, idx_t molecule_id, idx_t eqn_id)
{
  eqns[eqn_id][molecule_id].add_atom(atom_in);
}

/**
 * @brief evaluating the value of equation at a point
 * @param[in]  id of equation to calculate
 * @param[in]  index of depth
 * @param[in]  index of x direction
 * @param[in]  index of y direction
 * @param[in]  index of z direction
 */
real_t FASMultigrid::_evaluateEllipticEquationPt(idx_t eqn_id, idx_t depth_idx,
  idx_t i, idx_t j, idx_t k)
{
  real_t res = 0.0;
   
  for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
  {
    // value will end up being the value of a particular term in an equation
    real_t val = eqns[eqn_id][mol_id].const_coef;
    real_t pos_idx = H_INDEX(i, j, k,
       nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);

    if(rho_h[eqn_id][mol_id][depth_idx].pts > 0) // constant
      val *= rho_h[eqn_id][mol_id][depth_idx][pos_idx];
    
    
    for(idx_t atom_id = 0; atom_id < eqns[eqn_id][mol_id].atom_n; atom_id++)
    {
      atom & ad = eqns[eqn_id][mol_id].atoms[atom_id];
      
      if(ad.type == 1) // polynomial type
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        val *= pow(vd[pos_idx], ad.value);
      }
      else if(ad.type <= 4) // first derivative type
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        val *= derivative(i, j, k, vd.nx, vd.ny, vd.nz,
          der_type[ad.type][0], vd);
      }
      else if(ad.type <= 10)
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        val *= double_derivative(i, j, k, vd.nx, vd.ny, vd.nz,
          der_type[ad.type][0], der_type[ad.type][1], vd);
      }
      else
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        val *= laplacian(i, j, k, vd.nx, vd.ny, vd.nz, vd);
      }
    }
    res += val;
  }
  return res;
}

/**
 * @brief evaluate value of v * \partial F(u) / \partial u, storing coefficient a and b for interation
 *
 * @param id of equation which needs to be calculated
 * @param index of depth
 * @param coeficient a, stores the right hand side of iteration equation
 * @param coeficient b, stores the left hand coefficient for iteration
 * @param x grid index
 * @param y grid index
 * @param z grid index
 * @param id of variable in differentiation
 */
void FASMultigrid::_evaluateIterationForJacEquation(idx_t eqn_id,
  idx_t depth_idx, real_t &coef_a, real_t &coef_b,
  idx_t i, idx_t j, idx_t k, idx_t u_id)
{
  real_t dx = H_LEN_FRAC / (real_t)nx_h[depth_idx];

  // Currently can only deal with the case dx = dy = dz, needs to be generilized
  
  for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
  {
    real_t mol_to_a = 0.0, mol_to_b = 0.0;
    real_t non_der_val = eqns[eqn_id][mol_id].const_coef;
    real_t pos_idx = H_INDEX(i,j,k,nx_h[depth_idx],ny_h[depth_idx],nz_h[depth_idx]);

    if(rho_h[eqn_id][mol_id][depth_idx].pts > 0) // constant
     non_der_val *= rho_h[eqn_id][mol_id][depth_idx][pos_idx];

    // pre_val to help keep track of the result of terms with only one derivative
    for(idx_t atom_id = 0; atom_id < eqns[eqn_id][mol_id].atom_n; atom_id++)
    {
      atom & ad =  eqns[eqn_id][mol_id].atoms[atom_id];
      
      if(ad.type == 1) // polynomial type
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        if(u_id == ad.u_id)
        {
          mol_to_b = mol_to_b * pow(vd[pos_idx], ad.value)
            + non_der_val * ad.value * pow(vd[pos_idx], ad.value-1.0);
          non_der_val = non_der_val * pow(vd[pos_idx], ad.value);
          mol_to_a *= pow(vd[pos_idx], ad.value);
        }
        else
        {
          mol_to_b *= pow(vd[pos_idx], ad.value);
          mol_to_a *= pow(vd[pos_idx], ad.value);
          non_der_val *= pow(vd[pos_idx], ad.value);
        }
      }
      else if(ad.type <= 4) // first derivative type
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd =  damping_v_h[u_id][depth_idx];
        if(u_id == ad.u_id)
        {
          mol_to_a = mol_to_a * derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd)
            + non_der_val * derivative(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, der_type[ad.type][0], jac_vd);
          mol_to_b = mol_to_b * derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
          non_der_val =non_der_val * derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
        }
        else
        {
          non_der_val *= derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
          mol_to_b *= derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
          mol_to_a *= derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
        }
      }
      else if(ad.type <= 10)
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd =  damping_v_h[u_id][depth_idx];

        if(u_id == ad.u_id)
        {
          mol_to_a = mol_to_a * double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd)
            + non_der_val * (double_derivative(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, der_type[ad.type][0], der_type[ad.type][1], jac_vd) +
                             (ad.type <= 7) * double_der_coef[STENCIL_ORDER] * jac_vd[pos_idx] / (dx*dx));
          mol_to_b = mol_to_b * double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd)
            - (ad.type <= 7) * non_der_val * double_der_coef[STENCIL_ORDER]/(dx * dx);
          non_der_val = non_der_val * double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
        }
        else
        {
          non_der_val *= double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
          mol_to_a *= double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
          mol_to_b *= double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
        }
      }
      else
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd =  damping_v_h[u_id][depth_idx];

        if(u_id == ad.u_id)
        {
          mol_to_a = mol_to_a * laplacian(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, vd)
            + non_der_val * (laplacian(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, jac_vd) + 3.0 * double_der_coef[STENCIL_ORDER] * jac_vd[pos_idx] / (dx * dx));
          mol_to_b = mol_to_b * laplacian(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, vd)
            - non_der_val * 3.0 * double_der_coef[STENCIL_ORDER] / (dx*dx);
          non_der_val = non_der_val * laplacian(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, vd);
        }
        else
        {
          non_der_val *= laplacian(i, j, k, vd.nx, vd.ny, vd.nz, vd);
          mol_to_a *= laplacian(i, j, k, vd.nx, vd.ny, vd.nz, vd);
          mol_to_b *= laplacian(i, j, k, vd.nx, vd.ny, vd.nz, vd);
        }
      }
    }
    coef_a += mol_to_a;
    coef_b += mol_to_b;
  }

  return;
}

/**
 * @brief evaluate value of v * \partial F(u) / \partial u
 *
 * @param id of equation which needs to be calculated
 * @param index of depth
 * @param coeficient a, stores the right hand side of iteration equation
 * @param coeficient b, stores the left hand coefficient for iteration
 * @param x grid index
 * @param y grid index
 * @param z grid index
 * @param id of variable in differentiation
 */    
real_t FASMultigrid::_evaluateDerEllipticEquation(idx_t eqn_id, idx_t depth_idx, idx_t i, idx_t j, idx_t k, idx_t u_id)
{
  real_t res = 0.0;
  for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
  {
    real_t non_der_val = eqns[eqn_id][mol_id].const_coef, der_val = 0.0;
    // pre_val to help keep track of the result of terms with only one derivative

    real_t pos_idx = H_INDEX(i,j,k,nx_h[depth_idx],ny_h[depth_idx],nz_h[depth_idx]);

    if(rho_h[eqn_id][mol_id][depth_idx].pts > 0) // constant
      non_der_val *= rho_h[eqn_id][mol_id][depth_idx][pos_idx];

    for(idx_t atom_id = 0; atom_id < eqns[eqn_id][mol_id].atom_n; atom_id++)
    {
      atom & ad =  eqns[eqn_id][mol_id].atoms[atom_id];

      if(ad.type == 1) // polynomial type
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd =  damping_v_h[u_id][depth_idx];
        if(u_id == ad.u_id)
        {
          der_val = non_der_val * ad.value * pow(vd[pos_idx], ad.value-1.0) * jac_vd[pos_idx]
            + der_val * pow(vd[pos_idx], ad.value);

          non_der_val = non_der_val * pow(vd[pos_idx], ad.value);
        }
        else
        {
          non_der_val *= pow(vd[pos_idx], ad.value);
          der_val *= pow(vd[pos_idx], ad.value);
        }
      }
      else if(ad.type <= 4)// first derivative type
      {
        fas_grid_t & vd = u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd = damping_v_h[u_id][depth_idx];
        if(u_id == ad.u_id)
        {
          der_val = non_der_val * derivative(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, der_type[ad.type][0], jac_vd)
            + der_val * derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
          non_der_val = non_der_val * derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
        }
        else
        {
          non_der_val *= derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
          der_val *= derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], vd);
        }
      }
      else if(ad.type <= 10)
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd =  damping_v_h[u_id][depth_idx];

        if(u_id == ad.u_id)
        {
          der_val = non_der_val *  double_derivative(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, der_type[ad.type][0], der_type[ad.type][1], jac_vd)
            + der_val * double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd); 
          non_der_val = non_der_val * double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
        }
        else
        {
          non_der_val *=  double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
          der_val  *=  double_derivative(i, j, k, vd.nx, vd.ny, vd.nz, der_type[ad.type][0], der_type[ad.type][1], vd);
        }
      }
      else
      {
        fas_grid_t & vd =  u_h[ad.u_id][depth_idx];
        fas_grid_t & jac_vd =  damping_v_h[u_id][depth_idx];

        if(u_id == ad.u_id)
        {
          der_val = non_der_val * laplacian(i, j, k, vd.nx, vd.ny, vd.nz, jac_vd)
            + der_val * laplacian(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, vd);
          non_der_val = non_der_val * laplacian(i, j, k, jac_vd.nx, jac_vd.ny, jac_vd.nz, vd);
        }
        else
        {
          non_der_val *= laplacian(i, j, k, vd.nx, vd.ny, vd.nz, vd);
          der_val *= laplacian(i, j, k, vd.nx, vd.ny, vd.nz, vd);
        }
      }
    }
    res += der_val;
  }
  return res;
}

/**
 * @brief      initialize a grid to 0
 *
 * @param      grid    grid (array) to initialize
 * @param[in]  points  # points in array / on grid
 */
void FASMultigrid::_zeroGrid(fas_grid_t & grid)
{
  for(idx_t i = 0; i < grid.pts; i++)
    grid[i] = 0;
}

/**
 * @brief Shift all values in grid by a value
 * @details eg; grid[i] += shift for all i
 * 
 * @param grid grid to shift
 * @param shift amount to shift by
 * @param points # points in grid
 */
void FASMultigrid::_shiftGridVals(fas_grid_t & grid, real_t shift)
{
  #pragma omp parallel for
  for(idx_t i = 0; i < grid.pts; i++)
    grid[i] += shift;
}

/**
 * @brief "restrict" a fine grid to coarser grid
 * @details Restriction scheme:
 *  (1 given cell)*(1/8) + (6 adjacent "faces") * (1/16)
 *  + (12 adjacent "edges") * (1/32) + (8 adjacent "corners") * (1/64)
 * 
 * @param field_heirarchy field to restrict
 * @param fine_depth "depth" of finer grid
 */
void FASMultigrid::_restrictFine2coarse(fas_heirarchy_t grid_heirarchy, idx_t fine_depth)
{
  idx_t fine_idx = _dIdx(fine_depth);
  idx_t coarse_idx = fine_idx - 1;

  idx_t n_fine_x = grid_heirarchy[fine_idx].nx,
        n_fine_y = grid_heirarchy[fine_idx].ny,
        n_fine_z = grid_heirarchy[fine_idx].nz;
  idx_t n_coarse_x = n_fine_x / 2, n_coarse_y = n_fine_y / 2, n_coarse_z = n_fine_z / 2 ;

  fas_grid_t & fine_grid = grid_heirarchy[fine_idx];
  fas_grid_t & coarse_grid = grid_heirarchy[coarse_idx];
  idx_t i, j, k; // coarse grid iterator
  idx_t fi, fj, fk; // fine grid indexes

  #pragma omp parallel for default(shared) private(i,j,k)
  FAS_LOOP3_N(i, j, k, n_coarse_x, n_coarse_y, n_coarse_z)
  {
    fi = i*2;
    fj = j*2;
    fk = k*2;

    coarse_grid[H_INDEX(i,j,k,n_coarse_x, n_coarse_y, n_coarse_z)] =
      0.125 * fine_grid[H_INDEX(fi,fj,fk,n_fine_x, n_fine_y, n_fine_z)]
      + 0.0625 * (
        fine_grid[H_INDEX(fi+1,fj,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj+1,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj-1,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj,fk-1,n_fine_x, n_fine_y, n_fine_z)]
      ) + 0.03125 * (
        fine_grid[H_INDEX(fi+1,fj+1,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi+1,fj-1,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj+1,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj-1,fk,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi+1,fj,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi+1,fj,fk-1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj,fk-1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj+1,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj+1,fk-1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj-1,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi,fj-1,fk-1,n_fine_x, n_fine_y, n_fine_z)]
      ) + 0.015625 * (
        fine_grid[H_INDEX(fi+1,fj+1,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi+1,fj+1,fk-1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi+1,fj-1,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj+1,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi+1,fj-1,fk-1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj+1,fk-1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj-1,fk+1,n_fine_x, n_fine_y, n_fine_z)] +
        fine_grid[H_INDEX(fi-1,fj-1,fk-1,n_fine_x, n_fine_y, n_fine_z)]
      );

  } // end loop

}

/**
 * @brief interpolate a coarse grid to a finer grid
 * @details using a lot of "if" before updating to deal with the boundary probs when
 * n_coarse * 2 != n_fine 
 */
void FASMultigrid::_interpolateCoarse2fine(fas_heirarchy_t grid_heirarchy, idx_t coarse_depth)
{
  idx_t fine_idx = _dIdx(coarse_depth +1);
  idx_t coarse_idx = _dIdx(coarse_depth);

  idx_t n_coarse_x = nx_h[coarse_idx],
    n_coarse_y = ny_h[coarse_idx],
    n_coarse_z = nz_h[coarse_idx];
  idx_t n_fine_x = n_coarse_x *2, n_fine_y = n_coarse_y * 2, n_fine_z = n_coarse_z *2;

  fas_grid_t & coarse_grid = grid_heirarchy[coarse_idx];
  fas_grid_t & fine_grid = grid_heirarchy[fine_idx];
  idx_t i, j, k;
  idx_t fi, fj, fk;

  _zeroGrid(fine_grid);

  
  #pragma omp parallel for private(i, j, k, fi, fj, fk)
  FAS_LOOP3_N(i, j, k, n_coarse_x, n_coarse_y, n_coarse_z)
  {
    fi = i*2;
    fj = j*2;
    fk = k*2;

    real_t coarse_grid_val = coarse_grid[H_INDEX(i,j,k,n_coarse_x, n_coarse_y, n_coarse_z)];
    // loop over adjacent cells.
    for(idx_t  i_adj = -1; i_adj <= 1; ++i_adj )
      for(idx_t  j_adj = -1; j_adj <= 1; ++j_adj )
        for(idx_t  k_adj = -1; k_adj <= 1; ++k_adj )
        {
          idx_t fine_grid_loc = H_INDEX(fi + i_adj, fj + j_adj, fk + k_adj,
            n_fine_x, n_fine_y, n_fine_z);
          idx_t coarse_grid_loc = H_INDEX(fi + i_adj, fj + j_adj, fk + k_adj,
            n_coarse_x*2, n_coarse_y*2, n_coarse_z*2);

          if(i_adj == 0 && j_adj == 0 && k_adj == 0)
          {            
            #pragma omp atomic
            fine_grid[fine_grid_loc] += coarse_grid_val;
          }
          else if(fine_grid_loc == coarse_grid_loc)
          {
            real_t divisor = std::pow( 2.0,
              std::abs(i_adj) + std::abs(j_adj) + std::abs(k_adj) );
            #pragma omp atomic
            fine_grid[fine_grid_loc] += coarse_grid_val/divisor;
          }
        } // end for loop
  }

}

/**
 * @brief      Evaluate elliptic equation, stores in an array
 * 
 * @param      result_h  grid to store result on
 * @param      eqn_id    id of equation to deal with
 * @param[in]  depth     depth to evaluate at
 */
void FASMultigrid::_evaluateEllipticEquation(fas_heirarchy_t result_h, idx_t eqn_id, idx_t depth)
{
  idx_t i, j, k;
  idx_t depth_idx = _dIdx(depth);
  idx_t nx = nx_h[depth_idx], ny = ny_h[depth_idx], nz = nz_h[depth_idx];

  fas_grid_t & result = result_h[depth_idx];

  #pragma omp parallel for default(shared) private(i,j,k)
  FAS_LOOP3_N(i, j, k, nx, ny, nz)
  {
    idx_t idx = H_INDEX(i, j, k, nx, ny, nz);
    result[idx] = _evaluateEllipticEquationPt(eqn_id, depth_idx, i, j, k); 
  }
}

  
/**
 * @brief      Computes residual, stores result in "tmp" heirarchy
 * @param      residual_h  grid to store result on
 * @param      eqn_id    id of equation to deal with
 * @param[in]  depth     depth to evaluate at
 */
void FASMultigrid::_computeResidual(fas_heirarchy_t residual_h, idx_t eqn_id, idx_t depth)
{
  idx_t i, j, k;
  idx_t depth_idx = _dIdx(depth);
  idx_t nx = residual_h[depth_idx].nx, ny = residual_h[depth_idx].ny, nz = residual_h[depth_idx].nz;

  fas_grid_t & coarse_src = coarse_src_h[eqn_id][depth_idx];
  fas_grid_t & residual = residual_h[depth_idx];

  _evaluateEllipticEquation(residual_h, eqn_id, depth);

  #pragma omp parallel for default(shared) private(i,j,k)
  FAS_LOOP3_N(i,j,k,nx,ny,nz)
  {
    idx_t idx = H_INDEX(i, j, k, nx, ny, nz);
    residual[idx] = coarse_src[idx] - residual[idx];
  }
}

/**
 * @brief      Computes max residual for a equation
 * @param      eqn_id id of equation we deal with
 * @param[in]  depth  depth to compute residual at
 * @return residual
 */
real_t FASMultigrid::_getMaxResidual(idx_t eqn_id, idx_t depth)
{
  idx_t i, j, k;
  idx_t depth_idx = _dIdx(depth);
  idx_t nx = nx_h[depth_idx], ny = ny_h[depth_idx], nz = nz_h[depth_idx];
  fas_grid_t & coarse_src = coarse_src_h[eqn_id][depth_idx];

  real_t max_residual = 0.0;

  #pragma omp parallel for default(shared) private(j,k)
  FAS_LOOP3_N(i,j,k,nx,ny,nz)
  {
    idx_t idx = H_INDEX(i, j, k, nx, ny, nz);
    real_t current_residual = std::fabs(coarse_src[idx]
          - _evaluateEllipticEquationPt(eqn_id, depth_idx, i, j, k));
      
    #pragma omp critical
    {
      if(current_residual > max_residual)
        max_residual = current_residual;
    }
  }

  return max_residual;
}

/**
 * @brief get maximum residual among all equations
 *
 * @param depth to perform calculation
 * @return residual 
 */  
real_t FASMultigrid::_getMaxResidualAllEqs(idx_t depth)
{
  real_t max_for_all = 0;
  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    max_for_all = std::max(max_for_all, _getMaxResidual(eqn_id, depth));
  }
  return max_for_all;
}


/**
 * @brief      Compute coarse_src and u on a coarser grid
 * using tmp_h for some computations
 * @param id of equation
 * @param[in]  fine_depth  depth of grid to coarsen
 */
void FASMultigrid::_computeCoarseRestrictions(idx_t eqn_id, idx_t fine_depth)
{
  idx_t i, j, k;

  _restrictFine2coarse(u_h[eqn_id], fine_depth);

  _computeResidual(tmp_h[eqn_id], eqn_id, fine_depth);

  _restrictFine2coarse(tmp_h[eqn_id], fine_depth);

  _evaluateEllipticEquation(coarse_src_h[eqn_id], eqn_id, fine_depth - 1);

  idx_t coarse_idx = _dIdx(fine_depth -1);

  idx_t nx = nx_h[coarse_idx], ny = ny_h[coarse_idx], nz = nz_h[coarse_idx];

  fas_grid_t & coarse_src = coarse_src_h[eqn_id][coarse_idx];
  fas_grid_t & tmp = tmp_h[eqn_id][coarse_idx];

  #pragma omp parallel for default(shared) private(i,j,k)
  FAS_LOOP3_N(i,j,k,nx,ny,nz)
  {
    idx_t idx = H_INDEX(i, j, k, nx, ny, nz);
    coarse_src[idx] += tmp[idx];
  }
}

/**
 * @brief      Convert a grid containing an approximate solution
 *  to a grid containing the solution error, err = true - appx.
 *
 * @param      appx_to_err_h  grid heirarchy containing appx'n to convert
 * @param      exact_soln_h   heirarchy containing exact solution
 * @param[in]  depth          depth to perform computation at
 */  
void FASMultigrid::_changeApproximateSolutionToError(fas_heirarchy_t  appx_to_err_h,
    fas_heirarchy_t  exact_soln_h, idx_t depth)
{
  idx_t i, j, k;

  idx_t depth_idx = _dIdx(depth);
  idx_t nx = nx_h[depth_idx], ny = ny_h[depth_idx], nz = nz_h[depth_idx];

  fas_grid_t & appx_to_err = appx_to_err_h[depth_idx];
  fas_grid_t & exact_soln = exact_soln_h[depth_idx];

  #pragma omp parallel for default(shared) private(i,j,k)
  FAS_LOOP3_N(i,j,k,nx,ny,nz)
  {
    idx_t idx = H_INDEX(i, j, k, nx, ny, nz);
    appx_to_err[idx] = exact_soln[idx] - appx_to_err[idx];
  }
}

/**
 * @brief Compute and add in correction to fine grid from error
 * on coarser grid; replace error with appx. solution
 * 
 * @param err_h grid heirarchy containing error
 * @param err2appx_h heirarchy containing approximate solution
 * @param fine_depth depth of fine grid to correct
 */
void FASMultigrid::_correctFineFromCoarseErr_Err2Appx(fas_heirarchy_t err2appx_h,
          fas_heirarchy_t  appx_soln_h, idx_t fine_depth)
{
  idx_t i, j, k;
  idx_t coarse_depth = fine_depth-1;

  idx_t fine_depth_idx = _dIdx(fine_depth);

  idx_t n_fine_x = nx_h[fine_depth_idx], n_fine_y = ny_h[fine_depth_idx], n_fine_z = nz_h[fine_depth_idx];
  _interpolateCoarse2fine(err2appx_h, coarse_depth);

  fas_grid_t & err2appx = err2appx_h[fine_depth_idx];
  fas_grid_t & appx_soln = appx_soln_h[fine_depth_idx];

  #pragma omp parallel for default(shared) private(i,j,k)
  FAS_LOOP3_N(i,j,k, n_fine_x, n_fine_y, n_fine_z)
  {
    idx_t idx = H_INDEX(i, j, k, n_fine_x,n_fine_y,n_fine_z);
    // appx. solution in intermediate variable
    real_t appx_val = appx_soln[idx];
    // correct approximate solution with error
    appx_soln[idx] += err2appx[idx];
    // store approximate solution in err2appx
    err2appx[idx] = appx_val;
  }
}

/**
 * @brief Copy grid from one heirarchy to another
 * 
 * @param from_h copy from this heirarchy
 * @param to_h to this heirarchy
 * @param depth at this depth
 */
void FASMultigrid::_copyGrid(fas_heirarchy_t from_h[], fas_heirarchy_t to_h[], idx_t eqn_id, idx_t depth)
{
  idx_t depth_idx = _dIdx(depth);
  fas_grid_t & from = from_h[eqn_id][depth_idx];
  fas_grid_t & to = to_h[eqn_id][depth_idx];
  to = from;
}

/**
 * @brief iterative method to find a \lambda between 1 and zero,
 *        returning the largest value that satisfies
 *        norm less than the norm of F(u)
 * @param depth
 * @param norm
 */
bool FASMultigrid::_getLambda( idx_t depth, real_t norm)
{
  idx_t i, j, k, s;
  idx_t depth_idx = _dIdx(depth);
  idx_t nx = nx_h[depth_idx], ny = ny_h[depth_idx], nz = nz_h[depth_idx];
  real_t  sum = 0.0;

  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    fas_grid_t & u = u_h[eqn_id][depth_idx];
    fas_grid_t & damping_v = damping_v_h[eqn_id][depth_idx];
    FAS_LOOP3_N(i,j,k,nx,ny,nz)
    {
      idx_t idx = H_INDEX(i, j, k, nx,ny,nz);
      u[idx] +=  1.0 * damping_v[idx] ;
    }
  }
  
  for( s = 0; s < 100; s++)
  {
    //lambda = 1.0 - (real_t)s * 0.01; //should always start with \lambda = 1

    sum = 0.0;

    for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
    {
      fas_grid_t & coarse_src = coarse_src_h[eqn_id][depth_idx];
      #pragma omp parallel for default(shared) private(j,k) reduction(+:sum)
      FAS_LOOP3_N(i,j,k,nx,ny,nz)
      {
        idx_t idx = H_INDEX(i, j, k, nx,ny,nz);
        real_t temp = _evaluateEllipticEquationPt(eqn_id, depth_idx, i, j, k) - coarse_src[idx];
        sum += temp * temp;
      }
      
    }

    if(sum <= norm)  // when | F(u + \lambda v) | < | F(u) | stop
      return 1;

    for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
    {
      fas_grid_t & u = u_h[eqn_id][depth_idx];
      #pragma omp parallel for default(shared) private(j,k)
      FAS_LOOP3_N(i,j,k,nx,ny,nz)
      {
        fas_grid_t & damping_v = damping_v_h[eqn_id][depth_idx];
        idx_t idx = H_INDEX(i, j, k, nx,ny,nz);
        u[idx] -= (0.01) * damping_v[idx];
      }
    }
  }
  
  return 0;
}

/**
 * @brief perform Jacobian relaxation until a desired precision is reached
 * @details can be controled to use constrait or not, 
 * @param depth
 * @param norm of F(u)
 * @param parameter can control the converge speed
 * @param parameter can control the converge speed
 *
 */
bool FASMultigrid::_jacobianRelax( idx_t depth, real_t norm, real_t C, idx_t p)
{
  idx_t i, j, k;
  idx_t depth_idx = _dIdx(depth);
  idx_t nx = nx_h[depth_idx], ny = ny_h[depth_idx], nz = nz_h[depth_idx], cnt = 0;

  real_t   norm_r = 1e100,    norm_pre;

  //initilizing value of damping_v
  #pragma omp parallel for default(shared) private(j,k)
  FAS_LOOP3_N(i, j, k, nx, ny, nz)
  {
    for(idx_t eqn_id =0; eqn_id < u_n; eqn_id++)
      damping_v_h[eqn_id][depth_idx][H_INDEX(i,j,k,nx, ny, nz)] = 0.0;
  }
  
  while( norm_r >= std::min(pow(norm, (real_t)(p+1)) * C, norm)) 
  {
    //relax until the convergent condition got satisfy 
    norm_r = 0.0;
    norm_pre = 0.0;

    // TODO: parallelize
    for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
    {
      fas_grid_t & damping_v = damping_v_h[eqn_id][depth_idx];
      fas_grid_t & jac_rhs = jac_rhs_h[eqn_id][depth_idx];
      #pragma omp parallel for default(shared) private(j,k)
      FAS_LOOP3_N(i,j,k,nx,ny,nz)
      {
        idx_t idx = H_INDEX(i,j,k,nx,ny,nz);
        real_t coef_a =0, coef_b = 0, temp = 0;
        _evaluateIterationForJacEquation(eqn_id, depth_idx, coef_a, coef_b, i, j, k, eqn_id);
        for(idx_t u_id = 0; u_id < u_n; u_id++)
        {
          
          if(u_id != eqn_id)
            temp += _evaluateDerEllipticEquation(eqn_id, depth_idx, i, j, k, u_id);
        }
        damping_v[idx] = (coef_a - jac_rhs[idx] + temp)/ (-coef_b);
      }      
    }
    
    #pragma omp parallel for default(shared) private(i,j,k) reduction(+:norm_r)
    FAS_LOOP3_N(i,j,k,nx,ny,nz)
    {
      idx_t idx = H_INDEX(i, j, k, nx, ny, nz);
      for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
      {
        real_t temp = 0;
        fas_grid_t & jac_rhs = jac_rhs_h[eqn_id][depth_idx];
        for(idx_t u_id =0; u_id < u_n; u_id++)
          temp += _evaluateDerEllipticEquation(eqn_id, depth_idx, i, j, k, u_id);
        temp -= jac_rhs[idx];
        norm_r += temp * temp;      
      }
    }
          
    cnt++;

    if(cnt > 500 && norm_r > norm_pre) 
    {
      //cannot solve Jacobian equation to precision needed
      std::cout << "Unable to achieve a precise enough solution within "
                << cnt << " iterations.\n";
      return false;
    }
  }

  return true;
}

/**
 * @brief relax u using the inexact Newton iterative method
 * @param depth
 * @param max interation number
 */
void FASMultigrid::_relaxSolution_GaussSeidel( idx_t depth, idx_t max_iterations)
{
  idx_t i, j, k, s;
  idx_t depth_idx = _dIdx(depth);
  idx_t nx = nx_h[depth_idx], ny = ny_h[depth_idx], nz = nz_h[depth_idx];
  real_t   norm;

  for(s=0; s<max_iterations; ++s)
  {
    
    // move this precision condition to the beginning in case
    // perfect initial geuss causes infinite number of
    // iterations for function: _jacobianRelax()

    // set tolenrance precision, which should be smaller when grids become more coarse
    if(_getMaxResidualAllEqs( depth) < (relaxation_tolerance / pw2(1<<(max_depth_idx - depth_idx)) )) 
      break;

    if(relax_scheme == inexact_newton
        || relax_scheme == inexact_newton_constrained)
    {
      norm = 0.0;
      
      for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
      {
        fas_grid_t & jac_rhs = jac_rhs_h[eqn_id][depth_idx];
        fas_grid_t & coarse_src = coarse_src_h[eqn_id][depth_idx];
        
        #pragma omp parallel for default(shared) private(i,j,k) reduction(+:norm)
        FAS_LOOP3_N(i,j,k,nx,ny,nz)
        {
      
          idx_t idx = H_INDEX(i, j, k, nx, ny, nz);

          real_t temp = _evaluateEllipticEquationPt(eqn_id, depth_idx, i, j, k) - coarse_src[idx];

          norm += temp * temp;


          //evalue jac_source at right hand side of Jacobian linear equation
          jac_rhs[idx] = -temp;  
        }
      }
      if( _jacobianRelax(depth, norm, 1, 0) == false)
      {
        break;
      }
      
      // get damping parameter lambda
      if(_getLambda(depth, norm) == false)
      {
        std::cout<<"Can't fine suitable damping factor!!!\n";
        throw -1;
      }
    }

  } // end iterations loop

}


void FASMultigrid::_printStrip(fas_grid_t &out)
{
  idx_t i;
  idx_t nx = out.nx, ny = out.ny, nz = out.nz;
  std::cout << std::fixed << std::setprecision(15) << "Values: { ";
  for(i=0; i<nx; i++)
  {
    idx_t idx = H_INDEX(i,nx/4,ny/4, nx, ny, nz);
    std::cout << out[idx];
    std::cout << ", ";
  }
  std::cout << "}\n";
}


FASMultigrid::~FASMultigrid()
{
  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    for(idx_t depth = max_depth; depth >= min_depth; --depth)
    {
      idx_t depth_idx = _dIdx(depth);
    

      if(depth != max_depth) // can not delete the solution!!!!
        delete [] u_h[eqn_id][depth_idx]._array;
      delete [] coarse_src_h[eqn_id][depth_idx]._array;
      delete [] tmp_h[eqn_id][depth_idx]._array;
      delete [] damping_v_h[eqn_id][depth_idx]._array;
      delete [] jac_rhs_h[eqn_id][depth_idx]._array;
    }
    for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
    {
      for(idx_t depth = max_depth; depth >= min_depth; --depth)
      {
        idx_t depth_idx = _dIdx(depth);
        if(rho_h[eqn_id][mol_id][depth_idx].pts > 0)
        delete [] rho_h[eqn_id][mol_id][depth_idx]._array;
      }
    }
  }

}

/**
 * @brief      Initialize all constance function on all grids for all equations
 *
 */
void FASMultigrid::initializeRhoHeirarchy()
{

  //allocate space for rho first
  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
    {
      for(idx_t depth = max_depth-1; depth >= min_depth; --depth)
      {
        idx_t depth_idx = _dIdx(depth);
        if(rho_h[eqn_id][mol_id][depth_idx+1].pts > 0) //there is rho needs to be built
          rho_h[eqn_id][mol_id][depth_idx].init(
            nx_h[depth_idx], ny_h[depth_idx], nz_h[depth_idx]);

      }
    }
  }

  
  // restrict supplied rho to coarser grids
  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    for(idx_t mol_id = 0; mol_id < molecule_n[eqn_id]; mol_id++)
    {
      for(idx_t depth = max_depth; depth > min_depth; --depth)
      {
        idx_t depth_idx = _dIdx(depth);
        if(rho_h[eqn_id][mol_id][depth_idx].pts > 0) //there is rho needs to be built
          _restrictFine2coarse(rho_h[eqn_id][mol_id], depth);

      }
    }
  }

}

  
void FASMultigrid::VCycle()
{
  _relaxSolution_GaussSeidel(max_depth, max_relax_iters);

   std::cout << "  Initial max. residual on fine grid is: "
      << _getMaxResidualAllEqs(max_depth) << ".\n" << std::flush;

   idx_t depth, coarse_depth;

   for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
   {
     for(depth = max_depth; min_depth < depth; --depth)
       _computeCoarseRestrictions(eqn_id, depth);
     _copyGrid(u_h, tmp_h, eqn_id, min_depth);
   }

   for(coarse_depth = min_depth; coarse_depth < max_depth; coarse_depth++)
   {
     _relaxSolution_GaussSeidel(coarse_depth, max_relax_iters);
    
    std::cout << "    Working on upward stroke at depth " << coarse_depth
              << "; residual after solving is: "
              << _getMaxResidualAllEqs(coarse_depth) << ".\n" << std::flush;
    
    // tmp should hold appx. soln; convert to error
    for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
      _changeApproximateSolutionToError(tmp_h[eqn_id], u_h[eqn_id], coarse_depth);

    // tmp should hold error
    for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
      _correctFineFromCoarseErr_Err2Appx(tmp_h[eqn_id], u_h[eqn_id], coarse_depth+1);

    // tmp now holds appx. soln on finer grid;
    // phi_h now holds corrected solution on finer grid
   }

    _relaxSolution_GaussSeidel(max_depth, max_relax_iters);
    std::cout << "  Final max. residual on fine grid is: "
              << _getMaxResidualAllEqs(max_depth) << ".\n" << std::flush;
}

void FASMultigrid::VCycles(idx_t num_cycles)
{
  for(idx_t cycle = 0; cycle < num_cycles; ++cycle)
  {
    VCycle();
  }
  
  _relaxSolution_GaussSeidel(max_depth, 10);
  std::cout << "  Final solution residual is: "
      << _getMaxResidualAllEqs(max_depth) << "\n" << std::flush;
  
  for(idx_t eqn_id = 0; eqn_id < u_n; eqn_id++)
  {
    std::cout << " Solution for variable "<< eqn_id<<" has average / min / max value: "
              << u_h[eqn_id][max_depth_idx].avg() << " / " << u_h[eqn_id][max_depth_idx].min() << " / " << u_h[eqn_id][max_depth_idx].max() << ".\n" << std::flush;

  }
}

void FASMultigrid::printSolutionStrip(idx_t depth)
{
  _printStrip(u_h[0][depth]);
}


void FASMultigrid::setPolySrcAtPt(idx_t eqn_id, idx_t mol_id, idx_t i, idx_t j, idx_t k, real_t value)
{
  idx_t idx = H_INDEX(i, j, k,
    nx_h[max_depth_idx], ny_h[max_depth_idx], nz_h[max_depth_idx]);

  if(rho_h[eqn_id][mol_id][max_depth_idx].pts == 0)
  {
    rho_h[eqn_id][mol_id][max_depth_idx].init(
      nx_h[max_depth_idx], ny_h[max_depth_idx], nz_h[max_depth_idx]);
  }

  rho_h[eqn_id][mol_id][max_depth_idx][idx] = value;

}
  
} // namespace cosmo
